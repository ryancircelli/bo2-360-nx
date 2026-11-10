// Native renderer, steps C3, C4, C5c and C6 (see bo2zm_native_draws.h).
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// What it covers
//   - Shaders: modules of the SPIR-V library with the XenosRecomp interface
//     (shader_common.h and shader_recompiler.cpp): push constants with three
//     buffer addresses (VS, PS and shared constants), 2D, 3D and cubemap
//     textures in sets 0-2 and samplers in set 3, with no size limit;
//     specialization constant 0 with R11G11B10 normals (bit 0) and alpha test (bit 1).
//   - Vertices: the input comes from the fetches of the VS patched by D3D, each
//     element found by its destination register (C5b). Guest data is converted
//     to host words with the fetch constant's byte order, like the emulation
//     (spirv_translator_fetch.cpp), and uploaded to the frame buffer.
//   - Indices: VGT_DMA_BASE/SIZE or automatic, with VGT_INDX_OFFSET. Quad lists
//     become triangles v0 v1 v2 / v0 v2 v3 (primitive_processor.cpp).
//   - 2D textures by fetch constant: linear or tiled, formats 8, 8_8, 8888,
//     2_10_10_10, 16, 16_16, 16F, 32F, DXT1/3/5, DXT5A and DXN, with the fetch
//     constant's swizzle in the view. C2's resolved textures are sampled as they
//     are. The content is compared once per frame. The game's mip levels are
//     included (bo2zm_native_mipmaps), packed as on the Xbox 360; without them,
//     distant surfaces and foliage look grainy.
//   - State: blending, color mask, depth, stencil, culling, viewport (without the
//     half pixel: the shader adds it, g_HalfPixelOffset) and window scissor.
//   - Clipping disabled (draws in pixels, like the videos): viewport the size of
//     the render target and the transform in the VS with g_NdcScale/g_NdcOffset,
//     which only the library regenerated with our XenosRecomp fork (tools/xenosrecomp) has.
//
// Not covered (rejected or substituted, with the cause logged once)
//   Mips of 3D textures, vertex textures, signed or gamma textures, points,
//   rectangle lists, line loops and vertex formats without a direct Vulkan
//   equivalent.

#include "bo2zm_native_draws.h"
#include "bo2zm_gpu_vulkan.h"
#include "bo2zm_native_blend.h"
#include "../bo2zm_render_formats.h"
#include "../bo2zm_native_system.h"
#include "../shaders/bo2zm_debug_magenta_ps.h"  // diagnostics
#include "bo2zm_hitch_waits.h"

#include "bo2zm_native_vertex_dedupe.h"
#include "bo2zm_native_deferred.h"  // the job buffer's calls made by a recorder thread
#include "bo2zm_native_texture_pool.h"
#include "bo2zm_native_pso.h"  // bo2pso text lists and the cache folder
#include "../../bo2zm_switch.h"  // BuildId
#include "bo2zm_native_texture_mips.h"
#include "bo2zm_native_precompile.h"  // the precompile at start and its stamp file

#include "bo2zm_graphics_settings.h"
#include "../../bo2zm_preset.h"  // the graphics presets' texture detail
#include "../../bo2zm_benchmark.h"  // the benchmark A/B (bo2zm_native_nvk_copy_engine_b)
#include "../../bo2zm_perf_log.h"  // bo2zm_perf_log: synchronous compiles
#include "../../bo2zm_debug_options.h"  // the DEBUG page turns shortcuts off
#include "../../bo2zm_debug_watch.h"
#include "../../bo2zm_page_writes.h"  // write stamps for the vertex cache  // PC diagnostics, who writes the lighting volume
#include "../../bo2zm_volume_writes.h"  // the lighting volume's writer, hooked
#include "../../bo2zm_gpu_skin.h"  // GPU skinning, the skinning hooks' jobs
#include "../../bo2zm_cheats.h"  // in a match or not, for the pipeline cache's saves
#include "../../bo2zm_switch.h"  // CoresInUse, for the precompile's threads
#include "shaders/bo2zm_gpu_skin_cs.h"
#if __has_include("bo2zm_native_glow_energy_spirv.h") && __has_include("bo2zm_native_smooth_glow_spirv.h")
#include "bo2zm_native_glow_energy_spirv.h"
#include "bo2zm_native_smooth_glow_spirv.h"
#else
// The two variants of the glow bright pass are derived from a game shader and are not distributed with the
// sources. Without them the glow setting keeps the game's own shader.
#define BO2ZM_NO_VARIANTS_GLOW 1
static const uint32_t kSpirvGlowEnergy[1] = {0};
static const uint32_t kSpirvGlowSmooth[1] = {0};
#endif
#include "bo2zm_native_shaders.h"
#include "bo2zm_native_hooks.h"  // bo2zm_d3d_game_vegetation
#include "bo2zm_reflection_demand.h"  // reflection visibility
#include "bo2zm_shader_library.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/util.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <cstdlib>
#include <system_error>  // the bind thread
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). The method it picks for GCC (1) reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the write of the data
 * being hashed (strict aliasing). The texture key read keys[4] before writing it, and the same texture
 * was created several times. Same hash values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriorityOk (texture bind thread). That header deliberately does not
// include switch.h (same as in bo2zm_native_render.cpp).
#include "core/threading_switch.h"  // SDK src/ is on the include path (CMakeLists.txt)
#endif

/*
 * The final composite the renderer recognizes by hash (kHashComposite) is a single full-screen quad with
 * twelve texture samples, seven of them a radial blur. Turning it off removes them and leaves the center
 * tap. No BO2 shader has that hash, so in BO2 this does nothing.
 */
/*
 * Vegetation in the shadow map.
 *
 * The alpha-tested pixel shaders of the shadow pass are trees, bushes and fences: many of the pass's draws
 * but a small share of its triangles.
 *
 * The pass cost follows the triangle count, so removing them saves little GPU time but more CPU time, on a
 * thread that is close to a full core.
 *
 * Most of that saving is not geometry: it is the vegetation's alpha pixel shader, which in the shadow map
 * runs in full only to produce the cutout.
 *
 * What is lost: trees and fences stop casting shadows. Buildings and large objects keep theirs. It shows
 * where vegetation is dense.
 *
 * On by default: an improvement that depends on a line in a local toml is silently lost when the file is
 * missing.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_fast_tiling, true, "BO2",
                    "Native renderer: textures untiled 16 bytes at a time with the row computed "
                    "once. The first 2000 levels are checked against the usual path and, if one differs, it turns "
                    "itself off. false = the usual path, block by block");

/*
 * The FramebufferOf cache forgets destroyed views. It is keyed by handle values (render pass, 5 views
 * and size). When C2 destroys an image (GetResolved recreates it with another size or format at the same
 * address, and those textures end up as render targets through image swaps), a framebuffer kept past that
 * point could be returned for a new view that Vulkan happened to give the same handle, built on the dead
 * one. Destroy therefore notifies through ForgetView, and the framebuffers that use the view are destroyed.
 * false = nothing is forgotten until the destructor.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_framebuffers_forget_views, true, "BO2",
                    "Native renderer: when a C2 image is destroyed, the framebuffers that use its "
                    "view are destroyed too, so a reused handle does not return an old one. false = as before");

REXCVAR_DEFINE_INT32(bo2zm_native_hash_kb_per_frame, 6144, "BO2",
                     "Native renderer: KB of STABLE textures the ring checks again at most in one "
                     "frame; the ones that do not fit wait 1-2 frames (8 times in a row at most). Spreads out the "
                     "checks that used to fall on the same frame. 0 = no limit, as before");

/*
 * Sampled recheck of stable textures. See PrepareTexture and HashSample.
 * Enabled by default: its guard verifies itself and switches off at the first mismatch.
 */
REXCVAR_DEFINE_INT32(bo2zm_native_hash_sampling, 8, "BO2",
                     "Native renderer: STABLE textures are rechecked with the hash of a sample "
                     "(first and last 4 KB block and 1 in 8, base and mips) instead of all their bytes; if the sample "
                     "changes, the full path follows. N = 1 in N rechecks of each texture is still full, and the first "
                     "3000 compute both hashes: a single disagreement turns it off. 0 = always the full hash, as "
                     "before")
    .range(0, 64);

REXCVAR_DEFINE_BOOL(bo2zm_shadows_without_vegetation, true, "BO2",
                    "Do not draw alpha-tested geometry into the shadow map: trees, bushes and fences. They are many "
                    "of the pass's draws but few of its triangles, so it saves little GPU time and some CPU time. "
                    "Trees stop casting shadows");

/*
 * Radial blur of the final composite, removed by default (true).
 *
 * The Xbox 360 blurs the screen edges, and without it the image looks sharper. Restoring it together with
 * the 9-sample PCF made the GPU the bottleneck again, with twice as many long frames, and the difference is
 * hard to notice next to the 360 footage, so it stays removed. It only acts on the composite kHashComposite
 * names, which no BO2 shader has. false = keep the blur, as on the Xbox 360.
 */
/*
 * A full barrier on each side of every render pass (bo2zm_native_pass_barriers).
 *
 * The passes declare no subpass dependencies and the resolves copy out of a render target right after its pass
 * (and the next pass samples the copy) without a barrier: on NVK the GPU runs them in order anyway. MoltenVK
 * (argument buffers + residency sets) creates its resources without Metal hazard tracking and only orders work
 * through the fences it derives from vkCmdPipelineBarrier, so there a copy can read a depth or color target the
 * pass has not finished storing, and a draw can sample a resolved texture still being copied. The soft particles
 * read the scene depth that way: whole tiles of garbage depth turned sparks and embers into magenta, cyan and
 * yellow squares (the "purple particles"). Synchronization validation reports the same hazards
 * (SYNC-HAZARD-READ-AFTER-WRITE: vkCmdCopyImage reads an image written by the storeOp of vkCmdEndRenderPass).
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_pass_barriers,
#if defined(__APPLE__)
                    true,
#else
                    false,
#endif
                    "BO2",
                    "Native renderer: a memory barrier before and after every render pass, so copies and samples "
                    "of render targets wait for the pass (on by default on Mac: MoltenVK does not order them "
                    "by itself; without it particles can turn into magenta squares)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_no_blur, true, "BO2",
                    "Remove the radial blur from a final composite the renderer recognizes by hash (no BO2 shader "
                    "has it, so this does nothing in BO2). You lose the blur at the edges of the screen: the image is "
                    "sharper");

REXCVAR_DEFINE_BOOL(bo2zm_native_skip_shadows, false, "BO2",
                    "Native renderer (FPS test): does not record the draws into a depth-only target with a pitch of "
                    "1600 or more (BO2's 560-wide shadow target is not one); visible clipping");
// Enabled by default. On the console (A and B alternating every 30 s), the UBO intervals run 18-23 %
// faster than the neighbouring pointer intervals at the same draw load.
// Diagnostics: write one texture's fetch constant and the guest bytes of its base level (and the start of
// its mips) to dump_<address>.bin next to the executable, the first time a draw uses it. For checking a decoding
// against the bytes the game holds (a lightmap, for instance). Physical address in hex, e.g. 09544000.
REXCVAR_DEFINE_STRING(bo2zm_native_dump_texture, "", "BO2",
                      "Diagnostics: dump the guest bytes of the texture at this physical address (hex) once")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The address and size the texture dump used, for bo2zm_native_dump_memory="auto@SECONDS" (same texture, later).
std::atomic<uint64_t> g_bo2zm_dump_texture{0};
REXCVAR_DEFINE_INT32(bo2zm_native_dump_texture_after_s, 0, "BO2",
                     "Diagnostics: bo2zm_native_dump_texture waits this many seconds after startup")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_incremental_volume, true, "BO2",
                    "Native renderer: tiled 3D textures the game rewrites piecemeal (the model lighting "
                    "volume, 512x256x4) are updated by 4 KB pages: only the rows the changed pages touch are untiled "
                    "and uploaded. Checked against the whole read and turns itself off at the first disagreement. false "
                    "= read and upload the whole texture every time");
REXCVAR_DEFINE_INT32(bo2zm_native_volume_by_hash, 1, "BO2",
                     "Native renderer: how bo2zm_native_incremental_volume finds the pages that changed. "
                     "1 = each page's hash against the one of its last read copy (reads the 2 MB once); 0 = compares "
                     "them with that copy (reads 4 MB); 2 = alternates update by update, to measure both in the same "
                     "run (the C3 line gives the time of each)")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(bo2zm_native_constants_ubo, true, "BO2",
                    "Native renderer: shaders read their constants from dynamic UBOs (a constant bank on Maxwell) "
                    "instead of through a 64-bit pointer. Same bytes: the image does not change. Needs the shader "
                    "library with SPEC_CONSTANT_CONSTANTS_UBO; false goes back to the pointer");
REXCVAR_DEFINE_INT32(bo2zm_native_constants_ubo_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates the constants between the pointer (even spans) and "
                     "UBO (odd spans) every N seconds and logs each switch, to compare captures of a still scene in the "
                     "same run");
/*
 * Descriptor set 4 by differences (the work is done in NVK, see nvk_switch_set4 in patches/mesa-bo2zm.patch).
 *
 * vkCmdBindDescriptorSets for set 4 costs 1.3-1.4 us per call in gameplay and happens in 70-78 % of the
 * draws ("C6 substages" report), and it leaves four cbuf rebinds for the Draw. Almost all of it is NVK:
 * four writes to the root table (one per word of the dynamic descriptors, even when only the low part of
 * the address changes) and an 80-slot walk that dirties every cbuf of the set even when only one offset
 * changed. With the patch, NVK sends only the words that change and rebinds only the cbufs whose
 * descriptor changes. It is exact (the GPU ends up with the same bindings), and NVK checks it against
 * what it really bound in each slot: on a MISMATCH it fixes that draw, switches off for the rest of the run
 * and ControlSet4 reports it in the log as an error.
 * This cvar requests or withdraws it from NVK on each submission; with an unpatched Mesa (or on PC) it
 * does nothing. false = NVK binds the whole set, as usual.
 */
// On by default. Measured: one root-table write per bind instead of four, although the set 4 bind still
// costs 1.31-1.45 us (1.29-1.63 without it). In its first ~40 s the NVK guard checked 386,271 cbufs without a
// single difference, and the Draw rebound 38-45 % fewer cbufs. The guard samples with a process-wide count
// (p03 in patches/mesa-bo2zm.patch): sampling "1 in 4,096 draws of the buffer" would check nothing, since no
// buffer reaches 4,096 draws.
REXCVAR_DEFINE_BOOL(bo2zm_native_set4_by_differences, true, "BO2",
                    "Native renderer: set 4 (constants by UBO) by differences in NVK: only the "
                    "descriptors that change are sent and rebound. Same result (checks itself). Needs the Mesa with "
                    "nvk_switch_set4; false = as always");
REXCVAR_DEFINE_INT32(bo2zm_native_set4_by_differences_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates set 4 by differences (odd spans) and as "
                     "always (even spans) every N seconds and logs each switch, to compare C6 substages in the same "
                     "run. 0 = does not alternate");
/*
 * The draw path in NVK (p01-p04 and p06 in patches/mesa-bo2zm.patch).
 *
 * The ring is the bottleneck, and ~3.4 us of each draw is spent inside NVK: vkCmdDrawIndexed 1.4-1.6 us
 * per call, vkCmdBindPipeline 3-3.7 and set 4 1.3 ("C6 substages" report). Four exact improvements (the
 * GPU receives the same commands with the same data), each with its own guard in NVK that compares
 * against the usual path for the first 20,000 uses and then 1 in 1,024, and switches off on a MISMATCH:
 *   - emission: each command written in one go (Draw, cbuf rebinds, root table, BIND_VB), same bytes;
 *   - cbufs: only the slots that can get dirty, and no cbuf flush when nothing is dirty;
 *   - dynamic: only the dynamic state groups with dirty bits;
 *   - prefetch: cache hints (PRFM) for the pipeline and shaders before they are used; the app also hands
 *     the pipeline to NVK as soon as it knows it (after PipelineOf), several us before vkCmdBindPipeline.
 * Estimated for the four together: 0.6-1.5 us per draw (~1.1-2.8 ms of ring time per frame). The
 * per-part measurement (CNTPCT clock, 1 in N calls) gives the real breakdown in "C6 NVK by parts".
 * With an unpatched Mesa (or on PC) none of this does anything.
 */
REXCVAR_DEFINE_INT32(bo2zm_native_nvk_measure, 64, "BO2",
                     "Native renderer: measures by parts inside NVK 1 in N calls (power of 2; C6 NVK "
                     "by parts every 10 s). 0 = does not measure")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_measure_misses, false, "BO2",
                    "Native renderer (test): reads ahead what the pipeline bind and the shader flush will "
                    "use and measures it separately (how much is cache misses). Does extra work: measurement only");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_emission, true, "BO2",
                    "Native renderer: NVK writes each draw command in one go, with the same bytes "
                    "(checks itself). false = as always");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_cbufs, true, "BO2",
                    "Native renderer: NVK only looks at the cbuf slots that can get dirty and skips "
                    "the cbuf flush when nothing is dirty (checks itself). false = as always");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_dynamic, true, "BO2",
                    "Native renderer: NVK only emits the dynamic state groups with dirty bits "
                    "(checks itself). false = as always");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_prefetch, true, "BO2",
                    "Native renderer: cache hints (PRFM) for the pipeline and the shaders before "
                    "they are used, and the pipeline requested after PipelineOf. Changes nothing. false = no hints");
/*
 * Two more NVK changes (in patches/mesa-bo2zm.patch). On the console the ring thread, which sets the frame
 * rate, spent 1.77 us per pipeline bind copying the pipeline's fixed state field by field (vk_dynamic_graphics_state_copy)
 * while only ~1.4 state bits changed, and ~6 % of its time on vk_meta copies (image views per upload region, compute
 * QMDs, frees at command buffer reset under the global malloc lock).
 *   - bo2zm_native_nvk_fingerprint_copy: each pipeline carries one fingerprint per fixed-state bit and a bind copies
 *     only the bits whose fingerprint differs from what the command buffer holds. Exact; Mesa's guard compares it
 *     with the usual copy (first 20,000 binds, then 1 in 1,024) and switches it off on a MISMATCH.
 *   - bo2zm_native_nvk_copy_engine: bit 0, texture uploads (vkCmdCopyBufferToImage) on the copy engine, the path
 *     compressed and depth images already take on the Switch; bit 1, image-to-image copies too. On the GPU alone the
 *     copy engine is ~1.1 ms per frame slower for those copies, but through vk_meta each copy is made by the recorder
 *     thread at the frame's end and the ring waits for it: with both bits the console ran at 42.9 FPS against 39.2
 *     with uploads only (fully overclocked), the ring's wait for the recorder at the frame's end fell from ~4.1 ms to
 *     ~0.45 ms, and the recorder went from ~300 to ~165 ms/s busy. The default is bit 1 only (2): texture uploads on
 *     the copy engine make shapes drop out for single frames when the GPU is slow (handheld on battery, ~600 MHz;
 *     Buried's light shafts). The copy engine runs beside the 3D engine, and the uploads' barriers (UploadBarrier,
 *     bo2zm_native_targets.cpp) do not seem to hold it (probably because NVK's barriers wait for the 3D engine, not
 *     for copy engine work); through vk_meta, with those barriers, the bursts are smooth. Image-to-image copies on the
 *     copy engine show no such drop.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_fingerprint_copy, true, "BO2",
                    "Native renderer: when a pipeline is bound, NVK copies only the fixed-state groups "
                    "whose fingerprint changes (checks itself). false = the usual copy");
REXCVAR_DEFINE_INT32(bo2zm_native_nvk_copy_engine, 2, "BO2",
                     "Native renderer: NVK copies on the copy engine instead of vk_meta. Bit 0 = texture "
                     "uploads (buffer to image); bit 1 = image-to-image copies. 0 = everything as before")
    .range(0, 3);
REXCVAR_DEFINE_INT32(bo2zm_native_nvk_copy_engine_b, -1, "BO2",
                     "Test (Switch), with bo2zm_benchmark_ab_s: bo2zm_native_nvk_copy_engine's value in the A/B arms "
                     "with factor 2 on. -1 = not part of the A/B")
    .range(-1, 3);
/*
 * Kind views (struct nvk_bo2_kind_views, nvk_device_memory.h in patches/mesa-bo2zm.patch). A texture
 * placed in the texture pool is block-linear, so NVK gave it a GPU address range of its own with that PTE kind and
 * mapped its part of the pool block there: an AllocSpace and a MapBufferEx ioctl per texture, and an unmap and a
 * free when it goes. On the console, turning in place created ~116 new textures a second: 35-46 ms/s of address
 * allocations and 150-227 ms/s of map calls (each map 0.3 ms at first, ~2 ms later), and the binds thread spent
 * 100 ms/s inside vkBindImageMemory (almost nothing when standing still). With the views each pool block
 * is mapped once per kind, at the first texture of that kind, and every texture of that kind in it takes its address
 * from there: the same pages, one mapping.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_kind_views, true, "BO2",
                    "Native renderer: textures in the texture pool share one GPU mapping per pool block "
                    "(and memory kind) instead of a mapping each. false = a mapping per texture, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_kind_views_ab, false, "BO2",
                    "Test (Switch), with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_nvk_kind_views for the "
                    "textures bound in each arm (on = shared mappings)");
/*
 * Eviction by key. Near the texture limit the ring releases 4 cold textures a frame; picking them by
 * walking all of textures_, then walking views_ and textures_ again to drop them, cost ~8 % of a core
 * (ReleaseSomeFewCold, ReleaseImages and the frees under them). Instead, the coldest are gathered every
 * kColdScanEvery frames into a short list (re-checked when used) and released by key, their views found through
 * views_per_image_ (as ForgetImage does).
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_evict_by_key, true, "BO2",
                    "Native renderer: near the texture limit, cold textures are picked from a list "
                    "refreshed every 16 frames and released by key, without walking the whole cache every frame. "
                    "false = the full walks, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_evict_by_key_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_evict_by_key");
/*
 * The vertex copies in 64-byte steps with the source prefetched. In gameplay CopyVertices is the largest
 * item of the ring thread, 17.5 % of a core (~2.6 GB per 10 s in ~45,000 copies a second, ~6 KB each, into the
 * upload buffer without CPU cache, which the console writes at 3.5 GB/s). The loop GCC makes moves 16 bytes a step
 * with nothing ahead, ~1.4 GB/s per core, waiting on the guest memory it reads. On ARM64 the copy moves 64 bytes a
 * step and asks for the source 512 bytes ahead (the first lines at once). It is checked at start against the plain
 * loop on every byte order, size and offset; any difference keeps the plain loop.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_copy_neon, true, "BO2",
                    "Native renderer (Switch): vertex copies in 64-byte NEON steps with the source "
                    "prefetched ahead. false = the plain loop");
REXCVAR_DEFINE_INT32(bo2zm_native_copy_neon_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that switches bo2zm_native_copy_neon "
                     "(on in the arms with that factor on). 0 = not part of the A/B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_nvk_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates the four NVK improvements (on in odd "
                     "spans, off in even ones) every N seconds, to compare in the same run. 0 = does not alternate");

#if REX_PLATFORM_SWITCH
/*
 * The state of set 4 by differences lives in NVK (struct nvk_switch_set4 in nvk_cmd_buffer.h,
 * patches/mesa-bo2zm.patch). Same field order and types, with a version: it is a contract. Weak symbol:
 * with an unpatched Mesa the address is null and ControlSet4 reports it once.
 */
extern "C" {
struct NvkSwitchSet4Counts {
  uint64_t bindings_mismatch;
  uint64_t bindings_full;
  uint64_t writes_root;
  uint64_t dwords_root;
  uint64_t cbufs_dirty;
  uint64_t cbufs_saved;
  uint64_t draws;
  uint64_t checks;
  uint64_t mismatches;
};
struct NvkSwitchSet4 {
  int32_t version;
  int32_t requested;
  int32_t env;
  int32_t off;
  NvkSwitchSet4Counts total;
};
extern NvkSwitchSet4 nvk_switch_set4 __attribute__((weak));
}
static_assert(sizeof(NvkSwitchSet4) == 16 + 9 * 8, "NvkSwitchSet4 has to be the same size as in NVK");
#endif
#if REX_PLATFORM_SWITCH
/*
 * The draw path in NVK (struct nvk_switch_draw in nvk_cmd_buffer.h, p01 in patches/mesa-bo2zm.patch). Same
 * field order and types, with a version: it is a contract. Weak symbols: with an unpatched Mesa the
 * addresses are null and ControlDrawNvk reports it once.
 */
extern "C" {
struct NvkSwPart {
  uint64_t times;
  uint64_t ticks;  // CNTPCT_EL0, at ticks_per_second
};
struct NvkSwImprovement {
  int32_t requested;  // written by the app: 1 yes, 0 no
  int32_t off;  // 1: its guard saw a MISMATCH
  uint64_t uses;
  uint64_t validated;
  uint64_t mismatches;
  uint64_t no_check;
};
struct NvkSwitchDraw {
  int32_t version;
  int32_t measure;
  int32_t measure_misses;
  int32_t env;
  uint64_t ticks_per_second;
  NvkSwPart parts[16];   // enum nvk_sw_part
  uint64_t counts[13];    // enum nvk_sw_count
  NvkSwImprovement improvements[5];  // emission, cbufs, dynamic, set4_fast (not applied), prefetch
};
extern NvkSwitchDraw nvk_switch_draw __attribute__((weak));
void vk_switch_prefetch_pipeline(VkPipeline pipeline) __attribute__((weak));
}
static_assert(sizeof(NvkSwitchDraw) == 24 + 16 * 16 + 13 * 8 + 5 * 40, "NvkSwitchDraw: the same size as in NVK");
#endif
#if REX_PLATFORM_SWITCH
/*
 * Struct vk_bo2_copy_state (vk_graphics_state.h) and struct nvk_bo2_copies (nvk_cmd_buffer.h) in
 * patches/mesa-bo2zm.patch. Same field order and types, with a version: contracts. Weak symbols: with an older
 * Mesa the addresses are null and ControlCopiesNvk says so once.
 */
extern "C" {
struct VkBo2StateCopy {
  int32_t version;
  int32_t requested;
  int32_t off;
  int32_t padding;
  uint64_t copies;
  uint64_t no_hashes;
  uint64_t bits_copied;
  uint64_t bits_skipped;
  uint64_t checked;
  uint64_t mismatches;
  uint64_t pipelines;
};
struct NvkBo2Copies {
  int32_t version;
  int32_t engine;
  uint64_t uploads_engine;
  uint64_t regions_engine;
  uint64_t uploads_meta;
  uint64_t regions_meta;
  uint64_t copies_engine;
  uint64_t copies_meta;
};
struct NvkBo2KindViews {
  int32_t version;
  int32_t enabled;
  uint64_t views_created;
  uint64_t views_freed;
  uint64_t view_bytes;
  uint64_t binds_view;
  uint64_t binds_own;
  uint64_t failures;
};
extern VkBo2StateCopy vk_bo2_copy_state __attribute__((weak));
extern NvkBo2Copies nvk_bo2_copies __attribute__((weak));
extern NvkBo2KindViews nvk_bo2_kind_views __attribute__((weak));
}
static_assert(sizeof(VkBo2StateCopy) == 16 + 7 * 8, "VkBo2StateCopy: the same size as in Mesa");
static_assert(sizeof(NvkBo2Copies) == 8 + 6 * 8, "NvkBo2Copies: the same size as in NVK");
static_assert(sizeof(NvkBo2KindViews) == 8 + 6 * 8, "NvkBo2KindViews: the same size as in NVK");
#endif
/*
 * Sampler caches valid across frames.
 *
 * The trap to avoid: a resolved texture (a render target read back as a texture) that the game rewrites
 * every frame, such as a HUD panel drawn into its own target. If such a path set `valid_until = UINT64_MAX`
 * ("valid forever while the generation does not change"), then with three work slots, where the CPU runs
 * two frames ahead, the descriptor slot of an old view would be reused and the panel would smear; with two
 * slots the distance is too short for it to show.
 *
 * So those two paths set `valid_until = frame_` (see the comments in PrepareTexture), and the other paths
 * are tied to `texture.next`, the same policy that decides when the content is rechecked.
 *
 * What it buys: the cold path of the `textures` stage costs 13.5 us and runs 266 times per frame =
 * 3.6 ms. The hot path (2,893 hits) is indistinguishable from zero. With the cache valid across frames,
 * the cold ones drop to those expiring in that frame (~45-70), i.e. 2.6-3.0 ms.
 *
 * What to check, in motion (not paused): HUD panels the game rewrites every frame, the HUD and the
 * reflections. If anything looks blurry or delayed, disable this cvar in the toml.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_cache_across_frames, true, "BO2",
                    "Native renderer: the sampler caches stay valid across frames as long as the texture "
                    "content is not due for a check. false: they expire every frame, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Uploads the game's mip levels. With only the base level of each texture, distant surfaces looked grainy
// compared with the Xbox 360. This also fixes the base level of small textures with packed mips, which
// does not start at the base address.
REXCVAR_DEFINE_BOOL(bo2zm_native_mipmaps, true, "BO2",
                    "Native renderer: uploads the mip levels the game provides (as on the Xbox 360). false: only the "
                    "base level, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// A mip level read from the wrong place (packed tail offset, row or slice alignment) causes no Vulkan
// errors, only smudges in the distance. The game's mips are reductions of the base level, so their
// average color has to resemble the base level's.
/*
 * The texture cache limit. Without one, the cache never stops growing over a long session, and mipmaps make it
 * grow faster. Above the limit, the textures unused for the longest are evicted.
 *
 * Too high a limit runs the GPU out of memory: with 384 MB and no emergency path, the cache reached 313 MB after
 * 14 minutes, nvMapCreate failed even for 64 KB, and the screen went black with the audio still playing. Too low
 * a limit causes stutters: at 192 MB the cache was "near the limit" in every report, releasing cold textures a
 * trickle at a time only to upload them again (53 MB of uploads every 10 s, each re-upload a spike on the ring
 * thread, and 3.5-5 % of the frames over 50 ms), while the GPU memory budget showed ~900 MB untouched (478 MB
 * used of 1,375 MB budgeted).
 *
 * The emergency path (stop the GPU, release half the cache and retry) makes a generous limit safe. On the Switch
 * the limit is 512 MB, sized for bo2zm_internal_resolution = "auto" (720p handheld and 1080p docked): the cache
 * and the resolved render targets grow with the resolution, so the two go up together.
 */
#if REX_PLATFORM_SWITCH
constexpr int32_t kDefaultTexturesMbMax = 512;
#elif defined(__APPLE__)
// Apple silicon shares tens of GB with the GPU: room for four split-screen views' textures without churn.
constexpr int32_t kDefaultTexturesMbMax = 768;
#else
constexpr int32_t kDefaultTexturesMbMax = 384;
#endif
REXCVAR_DEFINE_INT32(bo2zm_native_textures_mb_max, kDefaultTexturesMbMax, "BO2",
                     "Native renderer: MB of textures above which the ones unused the longest (at least 120 frames) are "
                     "released, down to 75 %. If the game asks for them again, they are uploaded again. 0 = no limit, "
                     "as before")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Creating a new texture without stalling the ring thread.
 *
 * With the memory pool, creating a texture does not ask the system for memory, but vkBindImageMemory
 * still makes two ioctls: the plane's VA with its pte_kind and the mapping of the pool chunk
 * (nvk_image.c:1696-1710; all our textures are tiled with pte_kind GENERIC_16BX2,
 * nil/image.rs:439-440). In gameplay that is ~0.6 ms of wall time per texture, and entering a new zone
 * brings 33-45 at once: 19-27 ms of stalled ring in that frame.
 * With this, the ring does the CPU part and the bind thread does vkBindImageMemory while the ring carries
 * on with the draws; before closing the upload buffer it waits for whatever is missing and records the
 * barrier and the copy in that same buffer. The GPU receives the same thing in the same submission: no
 * placeholder textures and no lower mips.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_bind_thread, true, "BO2",
                    "Native renderer: the vkBindImageMemory of new textures (address reservation and "
                    "mapping on Horizon, ~0.6 ms each) runs on a separate thread and the ring keeps recording; before "
                    "submitting, whatever is missing is waited for. Same result (checks itself). false = on the ring")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_texture_bind_thread_priority, 0x2D, "BO2",
                     "Native renderer: priority of the texture bind thread. 0x2D, the ring's: above the "
                     "guest (0x3B) so a finished ioctl does not wait for a core, and outside the audio (0x2B) and "
                     "presentation (0x2C) bands. It sleeps in the ioctl almost all the time")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Hashing and untiling of new textures, off the ring thread.
 *
 * During zone-change stutters the ring spends 5.3-8.7 ms of the frame on new textures: the XXH3 of guest
 * memory (~37 %), untiling and byte swapping (~63 %, 1.8-3.1 ms per MB), plus the copy to the upload
 * buffer. With this, the ring only copies the bytes the hash covers (the same memory at the same point as
 * the inline path: a snapshot) and a thread does the hash, the untiling, the byte swap and the copy to the
 * upload buffer on that copy. Before submitting, the ring collects whatever is missing and does itself
 * whatever the thread has not started. The barrier and the copy are recorded where they always are: the
 * GPU receives the same bytes in the same submission, with no placeholder textures and no frame of delay.
 * It verifies itself (observing phase, then 1 in 128) and switches off with REXLOG_ERROR on any
 * MISMATCH. false = everything on the ring.
 */
// Off by default. Its guard trips at start-up (3 jobs not collected when switching upload buffers) and switches
// it off, and texture creation turned out not to cause gameplay stutters (56 us per texture on the ring); the
// zone-change stutter is limited by the game itself.
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_hash_thread, false, "BO2",
                    "Native renderer: the hash (XXH3), the untiling and the byte order of new "
                    "textures are done by a separate thread on a copy of guest memory taken on the ring; before "
                    "submitting, whatever is missing is collected. Same result (checks itself). false = on the ring, as "
                    "before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_texture_hash_thread_priority, 0x2E, "BO2",
                     "Native renderer: priority of the texture hash thread. 0x2E: below the ring (0x2D), "
                     "which preempts it as soon as it has work, and above the guest (0x3B), so the guest does not "
                     "starve it; outside the audio (0x2B) and presentation (0x2C) bands")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_texture_hash_thread_core, -2, "BO2",
                     "Native renderer: preferred core of the hash thread. -2 = automatic: core 2, or 1 if "
                     "the ring runs on 2 (on the ring's core, which is busy, it would barely run); -1 = the process "
                     "default; 0-2 = that one. Not exclusive")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_texture_hash_thread_mb, 16, "BO2",
                     "Native renderer: MB for the guest memory copies of a submission's new textures. "
                     "Whatever does not fit is prepared on the ring, as before")
    .range(4, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Measurement only: new textures whose content repeats a live one. See AnnotateContentTexture. It changes
 * no decision of the ring: it only counts, and reports every 10 s in the "C3 reuse by content"
 * line.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_reuse, true, "BO2",
                    "Native renderer (measurement only): counts how many new textures have the same "
                    "content and shape as another live one in the cache (the game reloads the packs at another address) "
                    "and how many of those others have gone unused for more than 120 frames. Changes nothing. false = "
                    "no counting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Pipeline prewarming. A pipeline that is not in the Vulkan cache has to be compiled before it can be used:
 * 68-159 ms each on the console (56 of them in a first launch, 5.2 s of stutter if done on the ring). It happens
 * the first time after any change to the shader library, the driver or the key, and on a fresh install. See
 * LoopPrewarm and the precompile at start (bo2zm_native_precompile.h).
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_async_pipelines, true, "BO2",
                    "Native renderer: a pipeline the driver has not compiled yet is compiled on a thread "
                    "of its own, and its draws are skipped until it is ready (a few frames), instead of the ring "
                    "stopping for it: switching to HIGH for the first time froze the game for 7.5 s, dozens of "
                    "compiles in a row. One in the driver's cache is created at once, as before. false = every "
                    "pipeline compiled on the ring");
// A first visit to a map compiles its ~230 pipelines on one thread at 155-192 ms each (~40 s with
// objects missing). In a match a second thread compiles no more (368 pipelines in the first minute against 365 with
// one, each taking twice as long: newlib's heap lock sets the pace there), while in the menus, where the cores are
// free, two threads do twice the work of one. One thread by default: the second one outside a match has not been
// measured on its own.
REXCVAR_DEFINE_INT32(bo2zm_native_compile_threads, 1, "BO2",
                     "Native renderer, with bo2zm_native_async_pipelines: how many threads compile pipelines. The "
                     "second only takes one outside a match (loading screens, menus) while two or more wait, so in a "
                     "match and with nothing queued it sleeps. 1 = one thread, as before")
    .range(1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_async_all, false, "BO2",
                    "Test helper, with bo2zm_native_async_pipelines: every pipeline the session creates goes to the "
                    "compile thread, those in the driver's cache too, as on a console that never compiled them (the "
                    "draws wait a frame or more). false = only those that need compiling")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// A map's first visit asks for ~200 pipelines in the match's first frame, and the compile thread used to take them in
// the order the frame asked: the scene's models first, then the post-processing chain and last the full-screen quad
// that copies the scene into the output image. With nothing to copy the scene, the screen stayed black but for the
// HUD (its pipelines come from the menus) for as long as the whole batch took, about a minute at a console's pace, and
// the world then appeared at once. The passes without a depth target (post-processing, the
// output copy, the HUD) need fewer than ten pipelines; compiled first, the picture is there in a few seconds and the
// models come in around it.
REXCVAR_DEFINE_INT32(bo2zm_native_compile_first, 1, "BO2",
                     "Native renderer, with bo2zm_native_async_pipelines: which pipelines go ahead in the compile "
                     "thread's queue (bits): 1 = draws in a pass without a depth target (post-processing, the copy of "
                     "the scene to the screen, the HUD), 2 = draws into a target 1280 or more wide, 4 = draws of 6 "
                     "indices or fewer, 8 = draws into targets narrower than 640. 0 = in the order the frame asks for "
                     "them")
    .range(0, 15)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_test_compile_delay_ms, 0, "BO2",
                     "Test helper, with bo2zm_native_async_pipelines: the compile thread waits this long after each "
                     "pipeline it compiles with a shader it has not compiled before, so a fast PC compiles at a "
                     "console's pace (300-500 ms each on a first visit; with bo2zm_native_test_async_all the others "
                     "pass at once, as the console's driver answers them from the shaders it keeps). 0 = no wait")
    .range(0, 5000);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_compile_log, false, "BO2",
                    "Test helper, with bo2zm_native_async_pipelines: a log line for each pipeline sent to the compile "
                    "thread (its pass, target, draw size and place in the queue) and for each one collected (how long "
                    "it waited and how many draws were skipped meanwhile)");
// Saving the pipeline file (~43 MB) during a match. On the OLED, a 390 ms frame and eight more of
// 107-323 ms fell inside the 2.2 s of one such write, which rewrote the whole cache because the prewarm list had one
// new record. Saving from the writer thread (mode 1) still stopped the ring for 0.5-1 s on a Switch Lite (frames of
// 742-1,006 ms with the ring working throughout, while the writer took the 51 MB cache from the driver), probably
// because Mesa's cache lock, held while the cache is serialized, blocks the ring's pipeline lookups. So by default
// the file is never saved in a match.
REXCVAR_DEFINE_INT32(bo2zm_native_pipelines_save_in_match, 0, "BO2",
                     "Native renderer: when the pipeline file is saved in a match. 0 = never (only in menus and "
                     "loading screens); 1 = once no new pipeline has come for 30 s, and only if the cache grew (a new "
                     "prewarm record alone waits for the next menu); 2 = as outside a match (after 64 new pipelines or "
                     "a minute with any). The writer thread takes the cache from the driver, and writes the file in "
                     "slices with pauses so the SD card stays free for the game's reads")
    .range(0, 2);
REXCVAR_DEFINE_INT32(bo2zm_native_test_pipelines_resave_s, 0, "BO2",
                     "Test helper: in a match, every this many seconds the whole pipeline file is taken from the "
                     "driver and written again, the odd ones at once and the even ones in slices (the [native] C6 "
                     "pipelines saved line says which), to see what a save costs. 0 = off")
    .range(0, 3600);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_fresh_pipelines, false, "BO2",
                    "Test helper: the session starts with an empty pipeline cache and no prewarm list, as a fresh "
                    "install does, and saves neither (cache/bo2zm_native_pipelines.bin is left as it is); on the "
                    "Switch Mesa's own shader cache is off too (bo2zm_switch.cpp): every pipeline is really compiled, "
                    "to measure the first visit to a map")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, bo2zm_native_test_cold_pipelines);  // bo2zm_native_precompile.cpp
REXCVAR_DEFINE_INT32(bo2zm_native_prewarm_threads, 2, "BO2",
                     "Native renderer, with bo2zm_native_prewarm_pipelines: 2 = a second prewarm thread walks the "
                     "list from its end, only while no match is on (menus, loading screens), until it meets the "
                     "first; what it compiles the first then finds in the driver's cache. 1 = one thread, as before")
    .range(1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_native_prewarm_lists, "cache/prewarm", "BO2",
                      "Native renderer: the folder, beside the NRO or the exe, whose *.bin prewarm lists are read at "
                      "start-up besides the pipelines file's own (tools/pipelines_list.py makes them)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_export_list, false, "BO2",
                    "Test helper: the session's prewarm list is also written to logs/bo2zm_prewarm_list.bin (at most "
                    "every 20 s while it grows), so a test can bring it back with the logs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_prewarm_pipelines, true, "BO2",
                    "Native renderer: at boot, a lowest-priority thread recreates in the Vulkan "
                    "cache the pipelines the ring created in earlier sessions (their list is in "
                    "cache/bo2zm_native_pipelines.bin), with the same function as the ring, and destroys them: when the "
                    "ring asks for them they are already compiled (without the 70-160 ms stutter per pipeline of the "
                    "first session after a change). Changes no pipeline and no draw. false = no prewarming (the list is "
                    "still saved)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Testing only. Pretends the GPU runs out of memory on one in N texture allocations, to check that the
// emergency path (stop the GPU, release half the cache and retry) really works. 0 does nothing, which is the
// normal setting.
REXCVAR_DEFINE_INT32(bo2zm_native_test_out_of_memory_every, 0, "BO2",
                     "Native renderer (tests only): fakes running out of GPU memory on one in N texture allocations, to "
                     "exercise the recovery. 0 = off")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_mips, false, "BO2",
                    "Native renderer (diagnostic): compares the average color of each mip level with the base's in "
                    "DXT1/3/5 and 8888 textures and logs the ones that do not look alike (a level read from somewhere "
                    "else). Does not change the image")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_depth_clamp, false, "BO2",
                    "Native renderer (tests only): depthClampEnable on every pipeline. Imitates what "
                    "NVK seems to do on the console with whatever is behind the far plane (the sun of the scene's "
                    "occlusion queries)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_smooth_glow_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 cycles the sky glow through original, natural and "
                     "soft (bo2zm_sky_glow) every N seconds and logs each switch, to compare captures of the same "
                     "place");
REXCVAR_DEFINE_BOOL(bo2zm_native_test_occlusion_always, false, "BO2",
                    "Native renderer (tests only): colorless draws of an occlusion query on a target "
                    "of 640 or more always pass the depth test. Tells apart a sun that covers no pixels from one hidden "
                    "by depth. Changes what the game sees")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * There is nothing to gain here, and this is why.
 *
 * A fit of the shadow pass's time against its triangle count, with a constant open area, leaves an intercept
 * near zero: with the maps open (all of them loaded with loadOp = LOAD) and zero triangles, the pass costs
 * nothing measurable. So on this GPU the loadOp = LOAD of a depth attachment is not paid for: Maxwell does not
 * load the tile up front like a tiled GPU, it reads on demand. The estimate of what the passes move for
 * nothing further below does not apply to the shadow map depth.
 *
 * On ZCULL: in NVK (nvk_cmd_draw.c, `use_zcull`) a pass with loadOp = CLEAR enables ZCULL even when the
 * image has no plane (ephemeral, without LOAD/STORE between passes, which a map drawn whole every time does
 * not need), while DONT_CARE, which this cvar uses, does not: the condition is
 * `zcull_plane || loadOp == CLEAR`, and DONT_CARE is neither. CLEAR has the same visual risk as DONT_CARE and
 * leaves the map at 1.0 (far) instead of garbage, so it is the better of the two. It still does not pay off:
 * the ZCULL ceiling here is that near-zero intercept, because the pass is pure geometry. For ZCULL to cover
 * the map, the renderArea has to span the whole map: the ZCULL region comes from render->area, not from the
 * image size.
 *
 * This only acts on a depth-only target 1600 or more wide (the same test as bo2zm_native_skip_shadows), and
 * BO2's 560x720 shadow target is not one.
 */
/*
 * On by default: an improvement that depends on a line in a local toml is silently lost when the file is
 * missing.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_shadow_pass_without_load, true, "BO2",
                    "Native renderer (FPS test): opens the shadow map pass (a depth-only target 1600 or more wide, "
                    "which BO2's is not) without loading its previous contents (loadOp = DONT_CARE). Measured: nothing "
                    "to gain, the depth loadOp is not paid on this GPU (the pass with 0 triangles costs almost "
                    "nothing)");
REXCVAR_DEFINE_INT32(bo2zm_native_skip_shadows_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 skips the shadows in the odd N-second spans and logs each "
                     "switch, to compare captures of the same place");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_repeated_vertices, false, "BO2",
                    "Native renderer (diagnostic): counts the vertex bytes that repeat address, size and content within "
                    "the same frame or against an earlier one");
// On the console, the vertex copy was ~3 of the ring thread's ~9 us per draw ("uploads" stage), and with the
// game's busy-waits removed there are free cores.
REXCVAR_DEFINE_BOOL(bo2zm_native_upload_thread, true, "BO2",
                    "Native renderer: the vertex copies into the upload buffer are done by a separate thread while the "
                    "ring thread keeps recording; they are waited for before submitting the work to the GPU and before "
                    "returning the read pointer to the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The ring does not wait for a copy thread that has no core (see WaitUploads).
 *
 * At the copy thread's creation priority (0x3B, below the two game threads at 0x3A) and with the CPU at
 * 278 % of 300, the thread sat ready without a core: in one stutter the ring waited 14.7 ms for it (3,965
 * draws), and only ~6-7 of those ms were copying, while the game spent 21.1 ms without room in the ring.
 * With the ring's help path (bo2zm_native_upload_help) the ring then always reached the copies first and
 * did all of them itself (1.4-1.8 GB, ~1 s every 10 s: ~3 ms per frame on the thread that is the bottleneck).
 */
/*
 * Copy thread priority. At 0x2E, like the hash thread, it takes the copies as soon as they are queued, on
 * another core; the ring (0x2D) preempts it if they share a core, and the help path only handles what is
 * left when the ring has to wait.
 */
REXCVAR_DEFINE_INT32(bo2zm_native_upload_thread_priority, 0x2E, "BO2",
                     "Native renderer: priority of the vertex copy thread. 0x2E: below the ring "
                     "(0x2D) and above the guest (0x3A-0x3B), so it copies while the ring records. 0x3B = as up to 186")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The copy thread on a different core from the ring. In one heavy stretch the thread only did 20 % of
 * the copies: the ring, with higher priority (0x2D) and at 94 %, did not leave it its core, and did them
 * itself while waiting (2.5-5 ms per frame). With its preferred core elsewhere, the thread takes CPU from the
 * game threads (0x3A, lower priority), which spend 97 % of their time waiting for the ring. Only the preferred
 * core is set: the affinity mask is untouched and the kernel can still move it (same as the hash thread).
 */
REXCVAR_DEFINE_INT32(bo2zm_native_upload_thread_core, -2, "BO2",
                     "Native renderer: preferred core of the vertex copy thread. -2 = one different "
                     "from the ring's; -1 = no preference (as up to 190); 0-2 = that one")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * 16-bit indices with hand-written NEON (Indices16). The compiler does not reliably vectorize that loop:
 * when it does not, the "indices" substage rises from 1.2-1.3 us to 2.95 us per draw, ~3 ms of ring time per
 * frame. Same results (the same indices in the same order, the same minimum and maximum). Guard: the first
 * 20,000 draws, then 1 in 4,096, are compared with the plain loop; on a MISMATCH the plain loop is kept for
 * the rest of the run.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_reuse_textures, true, "BO2",
                    "Native renderer: a draw whose pixel shader, fetch constants and texture caches did not change since "
                    "the previous draw reuses that draw's texture words instead of running the sampler loop (guarded: "
                    "checked against the loop, off for the session on any difference)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_indices_neon, true, "BO2",
                    "Native renderer: 16-bit indices are copied and measured (min and max) with "
                    "NEON, 16 per loop. Same result (checks itself). false = the usual loop")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_upload_help, true, "BO2",
                    "Native renderer: when the ring thread has to wait for the vertex copies, it "
                    "copies itself the ones the copy thread has not started yet and waits for the one in progress with "
                    "priority inheritance. Same data (checks itself). false = waits as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_diag_gray_empties, false, "BO2",
                    "Native renderer: textures not supported yet (cube, 3D, pending formats) sample gray instead of "
                    "zero (tests only)");
REXCVAR_DEFINE_BOOL(bo2zm_native_inv_tex_size, true, "BO2",
                    "Native renderer: shaders take 1/texture size from the constants instead of "
                    "asking the texture for it on each offset sample. Same arithmetic with the same number: the image "
                    "does not change. Needs a shader library regenerated with the new helper");
/*
 * Enabled by default: a single texel instead of the 3x3, measured and long enabled in a local toml. Three
 * cvars were only enabled through a local toml, and the toml overrides the default: shipping without that
 * line would lose the improvement without anyone noticing.
 */
/*
 * The cheap PCF was briefly disabled by default because, with a single shadow map sample, walls and door
 * doors showed diagonal bands (shadow acne) that the Xbox 360 does not have; from a distance they looked
 * blurry. The 9-sample PCF fixed nothing: the grid looked the same with 9 samples as with 1, because it was
 * shadow map acne, which the depth slope bias removes (bo2zm_native_shadow_bias_slope). The 9 samples
 * only cost GPU time: together with the blur, they made the GPU the bottleneck again. So the cheap PCF is
 * enabled by default again.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_cheap_pcf, true, "BO2",
                    "Native renderer: a single shadow map sample instead of the 3x3 half-texel pattern. The "
                    "shadow edge is a little less smooth (true by default: the bands on the "
                    "walls were acne and the depth bias removes them). Needs the regenerated shader library");

/*
 * Anisotropic filtering, beyond the Xbox 360 (bo2zm_native_anisotropic).
 *
 * The game requests trilinear without anisotropy on all its textures ("C4 filters requested": anisotropy 0,
 * bias 0), and the 435 sampling instructions of its 74 pixel shaders do not change that (all of them "use
 * the constant", read from the original binaries). That is why, on the 360 too, surfaces seen at a grazing
 * angle or from a distance look blurry and gain detail as you get closer: a white door seen at an angle,
 * for example. With N > 1, linear samplers with mips get anisotropy N (capped by the device). Single-level
 * ones (resolved targets, shadow map, screen effects) and point samplers stay as they were. It costs GPU time
 * in the scene: measured in "C2: GPU per Swap ... scene". 0 = trilinear, as on the 360.
 */
// 0 by default. At 8x it did not remove the streaks on the alley doors, and the scene went up ~2 ms of GPU
// time together with the 9 shadow samples.
REXCVAR_DEFINE_INT32(bo2zm_native_anisotropic, 0, "BO2",
                     "Native renderer: anisotropic filtering of the world textures (linear samplers "
                     "with mips). 0 = like the Xbox 360 (trilinear, blurry at an angle and in the distance); 2, 4, 8 or "
                     "16 = sharper, at a GPU cost")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * Alley door diagnostic (bo2zm_native_diag_min_mip).
 *
 * Door slats and brickwork showed streaks and ripples that the Xbox 360 does not have, and
 * they did not go away at native 720p nor with 8x anisotropy. With N > 0, textures with mips start N levels
 * lower even up close (the sampler raises its minLod). Standing still in front of a door: if with 1, 2
 * or 3 it looks blurry but with its slats in place, its mip levels are fine and the streaks come from
 * elsewhere (the shadow); if streaks, breaks or a pattern that is not the door's show up, that level is
 * uploaded wrong. It can be changed at runtime from the settings menu: it is part of the sampler key and,
 * when it changes, the per-texture sampler caches are invalidated. 0 = normal.
 */
/*
 * Our own depth bias in the shadow map (the acne on the alley doors).
 *
 * Doors and walls showed a fine grid of dots and diagonal streaks that the Xbox 360 does not have.
 * Diagnosed with captures taken standing in front of a door: without the shadow map the grid disappears;
 * with 9 samples or with 1 it looks the same; and with the minimum mip at 1, 2 or 3 the texture blurs as
 * expected but the grid stays just as sharp. It is shadow acne: the surface shadows itself. The game sets no
 * depth bias in that pass (0 "depth bias" lines in every log). Our own is added only to
 * the shadow map draws, and only if the game does not set one. Too high a value detaches the shadows from the
 * objects casting them. Both values can be changed at runtime from the settings menu. 0 and 0 = previous
 * behaviour.
 */
REXCVAR_DEFINE_INT32(bo2zm_native_shadow_bias_slope, 20, "BO2",
                     "Native renderer: slope-scaled depth bias in the shadow map, in tenths (20 = "
                     "2.0). Removes the acne (a diagonal dot grid on walls and doors). Too high lifts the "
                     "shadows off the objects. 0 = as before. Changes on the fly")
    .range(0, 100);
REXCVAR_DEFINE_INT32(bo2zm_native_shadow_bias_constant, 0, "BO2",
                     "Native renderer: constant depth bias in the shadow map, in thousands of 24-bit "
                     "units (1 = 1000). 0 = no constant. Changes on the fly")
    .range(0, 100);

REXCVAR_DEFINE_INT32(bo2zm_native_diag_min_mip, 0, "BO2",
                     "Diagnostic: textures with mips start N levels lower, also up close, to see "
                     "whether their mip levels are right. 0 = normal; 1, 2 or 3 to look. Changes on the fly")
    .range(0, 4);
/*
 * Splitting the frame into two submissions.
 *
 * Measured on the console: there is a single vkQueueSubmit per frame, at the end, from Present. The GPU
 * runs out of work from the end of one frame until the CPU sends the next, and that is a few ms of idle
 * time per frame ("gap between jobs", constant in steady gameplay).
 *
 * Submitting as soon as the shadow pass closes lets the GPU start on the shadows while the CPU records the
 * scene. It does not change a single pixel: it is the same work in two pieces.
 */
/*
 * Disabled by default. The idea above is sound but the measurement ruled it out: the gap between
 * submissions went up when the frame was split in two (two vkQueueSubmit calls = twice the GPU
 * start-up latency, and the second piece cannot start until the CPU finishes recording it). The default
 * follows the measurement: what helps ships enabled and what makes things worse ships disabled. The setting
 * stays so the test can be repeated.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_submit_after_shadows, false, "BO2",
                    "Native renderer: submit the work to the GPU as soon as the shadow pass closes, instead of "
                    "all together at the end of the frame. The GPU stops sitting idle waiting for the CPU to finish "
                    "recording");
REXCVAR_DEFINE_INT32(bo2zm_native_cheap_pcf_alternate_s, 0, "BO2",
                     "Native renderer: with N > 0 alternates every N seconds the shadow map sampling between "
                     "the 3x3 pattern and a single texel, and logs it. For A/B in the SAME session: comparing "
                     "between different sessions does not work because the stretch changes");
REXCVAR_DEFINE_INT32(bo2zm_native_inv_tex_size_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates every N seconds between taking 1/size "
                     "from the constants and asking the texture for it");
REXCVAR_DEFINE_INT32(bo2zm_native_per_draw_stats_s, 0, "BO2",
                     "Native renderer (diagnostic): with N > 0 measures one frame every N seconds with one "
                     "query per draw and splits the fragments by pixel shader");
REXCVAR_DEFINE_INT32(bo2zm_native_test_scissor, 0, "BO2",
                     "Native renderer (test): clips the draws of one category to 1x1 (1 shadows, 2 scene, 3 "
                     "cube, 4 postprocess, 5 the 'reflection' category: the scene at the internal resolutions below "
                     "1280, e.g. very low's). All the state and all the draws are sent but nothing is shaded: the time "
                     "difference is the per-draw floor. BREAKS THE IMAGE while it is set: measurement only");
REXCVAR_DEFINE_INT32(bo2zm_native_test_scissor_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates bo2zm_native_test_scissor's scissor "
                     "every N seconds");
/*
 * Separating the texture unit from the ALU (scene pass analysis).
 *
 * The game requests mipmapMode LINEAR on almost every sampler (mag/min/mip filters 1/1/1). On Maxwell a
 * trilinear sample takes the TMU two cycles and a bilinear one only one, so this halves the texture unit's
 * work without removing a single ALU instruction or shader sample. It is the only clean lever to decide
 * which of the two dominates the 18.94 ms of the scene. The jump between mip levels is visible when moving
 * away: for measurement only.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_test_point_mip, false, "BO2",
                    "Native renderer (test): the filter BETWEEN mip levels goes from linear to point (trilinear "
                    "-> bilinear). The TMU does half the work and the ALU does not change: if the scene gets faster, "
                    "sampling is the limit; if it does not move, the ALU is. The mip step shows when moving away: "
                    "measurement only");
REXCVAR_DEFINE_INT32(bo2zm_native_test_point_mip_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates bo2zm_native_test_point_mip every N seconds "
                     "(both samplers live in the cache, no stalls when switching)");
/*
 * Do not upload the same vertices twice in the same frame.
 *
 * Each draw copies the [vmin..vmax] range of each binding to the upload buffer, byte-swapped. There was no
 * cache: the same piece of geometry was copied again in full every time it was drawn. Measured on PC with
 * bo2zm_native_diag_repeated_vertices:
 *     5954.9 MB copied; 2584.2 MB repeated within the same frame and 3329.2 MB equal to an earlier
 *     frame  ->  99.0-99.6 % of the bytes are byte-for-byte repeats, across eight reports.
 * On the console that is 8.4 MB of vertices per frame at 2518 MB/s into uncached memory = 3.3 ms of
 * writing alone, split between the ring and the copy thread.
 *
 * This only does the safe half: repeats within the same frame (41-49 % of the bytes). And it is exact, not a
 * gamble: on the Xbox 360 the GPU reads the vertices when it executes the draw, so the game cannot rewrite a
 * range that is already referenced without first synchronizing with the GPU. The only point where it can is
 * a guest wait (WAIT_REG_MEM), and there everything recorded is forgotten (g_syncs_ring).
 *
 * If geometry ever looks stuck or stretched, this is the first thing to disable.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_dedupe_indices, true, "BO2",
                    "Native renderer: if two draws of the same frame use the same 16-bit index range, it is scanned "
                    "and uploaded only once (DedupeIndices in bo2zm_native_vertex_dedupe.h); checked against a fresh "
                    "scan for the first 20,000 reuses");
REXCVAR_DEFINE_BOOL(bo2zm_native_dedupe_keep_on_flush_waits, true, "BO2",
                    "Native renderer: the upload dedupe (vertices and indices) keeps what it recorded across the "
                    "GPU's cache flush waits (g_syncs_on_flush_waits); checked by hash on the first 20,000 reuses and "
                    "every 512th frame. false = forget at every WAIT_REG_MEM, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The vertex cache (bo2zm_native_vertex_cache), off by default. On the console reading the guest bytes to
 * check them costs about what copying them does (~2 GB/s either way, by XXH3 or by sums): neutral on a straight
 * benchmark route (+1.5, +0.2 %) and -2.1 % when turning in place, where the view keeps bringing ranges in and out
 * and the pools refill ~100 MB per 10 s. It stays for the write-stamp variant below, which does not read every byte.
 *
 * The in-frame dedupe (bo2zm_native_vertex_dedupe.h) stops a range from being copied twice in one frame. This covers
 * the other half: on the PC 51-59 % of the copied bytes are equal to an earlier frame's, and on the console the
 * copies still cost ~13 % of a core (2,237 MB per 10 s), mostly writes into the upload buffer's uncached memory.
 * Here a range (guest physical address, bytes, byte order) that the dedupe did not find is hashed (XXH3 of its guest
 * bytes, read from cached memory):
 *   - if a persistent copy with that hash exists, the draw binds it and nothing is copied;
 *   - if the hash is the one this range had the last time it was drawn (it did not change), it is copied once
 *     into the persistent buffer and bound from there;
 *   - otherwise it is copied into the upload buffer as usual, and its hash is remembered.
 * The persistent copies live in two buffers ("pools") of bo2zm_native_vertex_cache_mb each, filled forwards and
 * never written twice while the GPU may read them. When the one being filled is full, the cache switches to the
 * other: the full one's copies are dropped (generation), and it is only filled again after 8 more submissions (the
 * renderer has 3 work slots, and a slot is reused only once its previous submission has finished), so no draw
 * still in flight can read a region being rewritten. A hit binds the pool's buffer instead of the upload buffer
 * (also on the base-zero path); the dedupe remembers which buffer.
 *
 * Exact as far as XXH3 is: a different range of bytes with the same 64-bit hash would be needed to bind stale
 * vertices. Guard: the first 2,000 hits and then 1 in 1,024 copy the guest bytes again into a scratch buffer and
 * compare them with the pool copy; one difference turns the cache off for the rest of the run (MISMATCH in the log).
 */
namespace bo2zm::native {
std::atomic<uint32_t> g_probe_vertex_address{0};
}

REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache, true, "BO2",
                    "Native renderer: vertex ranges whose guest bytes did not change since they were "
                    "last drawn are copied once into a persistent buffer and bound from there, instead of being "
                    "copied into every frame's upload buffer. With bo2zm_native_vertex_cache_by_writes (the default) "
                    "unchanged means no write stamped on their pages: +5.4 % on the console; with "
                    "hashes it cost about what it saved (#83, #88). false = copied every frame");
REXCVAR_DEFINE_INT32(bo2zm_native_vertex_cache_mb, 96, "BO2",
                     "Native renderer: size in MB of each of the vertex cache's two persistent buffers")
    .range(4, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache_sum, true, "BO2",
                    "Native renderer: the vertex cache checks a range with Fletcher sums of its 16-byte blocks "
                    "(ChangeSum, no multiplies) instead of XXH3 (~2 GB/s on the console). false = XXH3");
REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache_sum_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 2 is bo2zm_native_vertex_cache_sum (on = ChangeSum, "
                    "off = XXH3)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_vertex_cache (on = the cache, off "
                    "= copied every frame)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The vertex cache without hashing. A write watch on the PC over the pages the vertex copies read found
 * that 98 % of the bytes copied came from pages nothing wrote in 45 s of Town. With this, a range's persistent copy
 * stays valid while none of its pages has a write stamp newer
 * than the copy (bo2zm_page_writes.h: the game's copy and fill routines, the skinning hooks and memexport stamp what
 * they write), so its bytes are never read again. A sample of the hits is read back (the first 5,000, then 1 in
 * 128): a range whose bytes changed without a stamp marks its pages as always written.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache_by_writes, true, "BO2",
                    "Native renderer: with bo2zm_native_vertex_cache, a persistent vertex copy stays "
                    "valid while no write was stamped on its pages since (no hashing). false = the hash checks")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The same for 16-bit index ranges (IndexCacheByWrites). On the console the index scan (minimum and
 * maximum, byte order) takes 0.72-0.79 us per draw and the index upload 0.29-0.34 us per draw: ~1.4 ms of the ring
 * thread per frame, for index buffers that, like the vertices above, are mostly static geometry. A range drawn
 * twice with no write stamped on its pages in between is converted once into the vertex cache's pools, with its
 * minimum and maximum; later draws bind that copy and skip the scan and the upload. On the console: +1.1 %, ~290,000
 * hits per 10 s, and a handful of ranges that change without a stamp at match start, which the guard catches. On by
 * default.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_index_cache, true, "BO2",
                    "Native renderer: with the vertex cache's pools and write stamps, a 16-bit index "
                    "range is converted once and bound from there while its pages are not written (no scan, no "
                    "upload). false = scanned and uploaded every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_index_cache_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_index_cache (on = cached, off = "
                    "scanned every draw)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * On the console a vertex cache hit takes 0.57-0.62 us per binding, and the index stage still takes
 * 0.53-0.65 us per draw with the index cache hitting: neither copies anything, so that is the lookup itself. The
 * table is 65,536 sets of four 80-byte entries (~20 MB), so each lookup is probably a cache and TLB miss to DRAM, plus
 * the write stamps' line. With this, each draw's sets and stamp lines are prefetched as soon as its
 * ranges are known: the index range's at the start of the draw, the vertex bindings' ~2.5 us before their lookup
 * (the texture stage runs in between).
 */
// The ring thread's cache misses. Measured on the OLED: the recorder queue in batches, +0.5 % and 1.2 % of
// a core less on the ring thread; vertex inputs by VS generation, +0.3 % and 0.6 % of a core less. Both on by default.
// (Smaller dedupe tables gained nothing, and searching the vertex caches by set heads cost 0.2 %.)
REXCVAR_DEFINE_BOOL(bo2zm_native_deferred_batch, true, "BO2",
                    "Native renderer: the ring thread hands its queued Vulkan calls to the recorder in "
                    "512-byte batches and reads the recorder's progress only when out of room")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_deferred_batch_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 2 is bo2zm_native_deferred_batch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The ring thread's memset, memcpy and memcmp of a draw's 688-byte shared words. Measured on the OLED over
// 15 minutes: fewer copies +0.2 % (0.6 % of a core less on the ring), the double buffer +0.1 % (0.3 % less); both on.
REXCVAR_DEFINE_BOOL(bo2zm_native_fewer_copies, true, "BO2",
                    "Native renderer: a draw that reuses the previous one's texture words skips their "
                    "zeroing, copy back and fetch snapshot, and the vertex sources are not zeroed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_fewer_copies_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_fewer_copies")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_shared_swap, true, "BO2",
                    "Native renderer: a draw's shared words are built in one of two buffers and a change "
                    "swaps them, instead of copying the block to the previous draw's")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_shared_swap_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 2 is bo2zm_native_shared_swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_input_front, true, "BO2",
                    "Native renderer: a draw's vertex input found by its VS microcode's generation in a "
                    "small table first (no gather, hash and map walk when a VS comes back)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_input_front_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_input_front")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_prefetch_cache_lines, false, "BO2",
                    "Native renderer: prefetch the vertex and index caches' table lines and write stamps "
                    "of each draw early. false = looked up cold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_prefetch_cache_lines_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_prefetch_cache_lines")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Copy only the float constants a shader's microcode reads (ShaderInput::constants_runs, from xenia's
 * analysis). Shaders that address constants dynamically still get the whole block. On the console: +2.2 %, and the
 * VS constant traffic falls from 1.42 to ~0.8 GB per 10 s. On by default.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_constants_used, true, "BO2",
                    "Native renderer: a draw copies only the float constants its shaders read (not "
                    "dynamically addressed ones). false = the whole block up to the highest register")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_constants_used_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_constants_used")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The write stamps' 64 KB region summary (bo2zm_page_writes.h).
REXCVAR_DEFINE_BOOL(bo2zm_native_stamp_regions, false, "BO2",
                    "Native renderer: the vertex, index and texture caches check a range's write stamps by "
                    "64 KB region first. false = page by page")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_stamp_regions_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_stamp_regions")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Texture rechecks by write stamps. On the console the texture stage costs, per sampler, 0.17 us for a
 * register hit, 0.40 us for a table hit and 58.7 us for a trip through PrepareTexture; there are ~45 such trips per
 * frame (~2.6 ms of the ring thread), and ~90 % of them are a stable texture's periodic recheck (every 32-39 frames:
 * a sampled or full XXH3 of its guest bytes). With this, a stable texture's due recheck is skipped while no page it
 * reads has a write stamp since its last check (TextureUnwritten). On the console: +2.3 % in an A/B, ~2,200 rechecks
 * skipped per 10 s, and no change missed by the stamps.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_by_writes, true, "BO2",
                    "Native renderer: a stable texture's periodic recheck is skipped (no hashing) while "
                    "no write was stamped on its pages since its last check. false = always hashed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_by_writes_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_texture_by_writes (on = skipped "
                    "by stamps, off = hashed)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The same for young textures. A new or changed texture is rechecked 1, 2 and 4 frames after its upload
 * before it counts as stable (interval 8), each time with a full hash: even with bo2zm_native_texture_by_writes on,
 * that is ~350-550 full hashes a second on the console (59-83 us each), ~120 new and ~90 changed textures a second
 * times those three. With this, the skips start after the first recheck that finds it unchanged (interval 2). A
 * texture that changes every frame (a video) never gets there. On the console: ~1,150 young skips per 10 s, +0.4 %
 * (inside the noise), no change missed by the stamps. On by default, since it only removes work.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_by_writes_young, true, "BO2",
                    "Native renderer: with bo2zm_native_texture_by_writes, the skips start after one "
                    "recheck that finds the texture unchanged (interval 2) instead of three (interval 8)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_by_writes_young_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_texture_by_writes_young (on = from "
                    "interval 2, off = from interval 8)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * New textures that take a cold texture's image (ReuseByContent). In Town ~120 textures a second are new,
 * at 150-370 us each on the ring thread, and ~30 % of them have the same guest bytes and shape as a live texture
 * unused for more than 120 frames (bo2zm_native_diag_reuse). On the console ~30 % of new textures took a cold image,
 * for +0.3 % (inside the noise), so it is off by default.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_reuse, false, "BO2",
                    "Native renderer: a new texture with the same guest bytes and shape as a texture "
                    "unused for 120 frames takes that one's image (nothing created, read or uploaded). false = always "
                    "a new image")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_reuse_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_texture_reuse (on = reused, off = "
                    "a new image)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_dedupe_vertices, true, "BO2",
                    "Native renderer: if two draws of the same frame ask for the same vertex range, it is "
                    "uploaded only once. The C6 report logs hits and MB saved");

/*
 * No vkCmdBindVertexBuffers in single-binding draws.
 *
 * Each draw copies its vertices to a new spot in the upload buffer, so its binding changed almost every
 * time: in gameplay, 0.32-0.46 us per call in 66-98 % of the draws (0.21-0.34 us per draw). In NVK that is
 * three wrapper functions (vk_common_CmdBindVertexBuffers -> 2EXT -> 3KHR) and 5 words with an MME macro per
 * binding.
 *
 * With a single binding, the copy is allocated at a multiple of the stride, the upload buffer stays bound
 * at 0 and the draw is shifted with vertexOffset (indexed) or firstVertex (non-indexed). The GPU reads the
 * same bytes: (offset / stride + i - vmin) * stride = offset + (i - vmin) * stride. The library's shaders do not
 * read SV_VertexID (XenosRecomp only declares it with UNLEASHED_RECOMP and the library is generated with
 * BO2ZM_RECOMP), so the base index is not visible anywhere. NVK passes vertexOffset/firstVertex as is to
 * the draw macro on every Draw, with or without this change: recording the draw costs the same.
 *
 * These still bind as usual: draws with two or more bindings, those that reuse (dedupe) a copy that does not
 * start at a multiple of their stride, and the deferred sky, which saves and replays its own binding (and
 * when emitted leaves bindings_recorded_ at 0, so the next draw binds at 0 again).
 *
 * Guard (CheckBaseZero): the first 200,000 draws on this path, then 1 in 4,096, redo the computation
 * backwards in 64 bits and check the limits. On a single difference, that draw binds as usual, MISMATCH
 * is written to the log and the path switches off for the rest of the run. What the guard cannot see is the GPU:
 * if anything looked wrong, bo2zm_native_zero_base_vertices = false restores the usual path.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_zero_base_vertices, true, "BO2",
                    "Native renderer: in draws with a single vertex binding, the upload buffer stays "
                    "bound at 0 and the draw is offset with vertexOffset/firstVertex, without a vkCmdBindVertexBuffers "
                    "per draw. The GPU reads the same bytes. false = as before");

REXCVAR_DEFINE_BOOL(bo2zm_native_ps_alpha_only, true, "BO2",
                    "Native renderer: in passes without a color target, compiles the pixel shader "
                    "without the color writes. The image does not change (Vulkan discards them) and the driver removes "
                    "as dead whatever only fed the color: the alpha test remains");
REXCVAR_DEFINE_INT32(bo2zm_native_ps_alpha_only_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates every N seconds compiling the pixel "
                     "shader with and without the color writes in colorless passes");

/*
 * Depth test before shading, where it can be done without changing the image.
 *
 * The problem. The scene is pure shading, with every screen pixel shaded several times over, and our own
 * counter says that a good share of the color draws prevent early rejection: they have an alpha test or a
 * real kill.
 *
 * Why that costs so much. A pixel shader that can discard forces the hardware to shade first and test
 * depth afterwards (late-Z): otherwise a discarded fragment would already have written its Z. So everything
 * with alpha (smoke, particles, glass, decals) is shaded in full even when it is behind a building. In NVK
 * that is literal: nvk_shader.c only sets SET_API_MANDATED_EARLY_Z when the module declares
 * EarlyFragmentTests, and our modules never declare it.
 *
 * The part that can be fixed, and why it is pixel-identical. The reason for late-Z is the Z write, not the
 * test. If the draw writes neither depth nor stencil, moving the test earlier cannot corrupt anything: there
 * is nothing extra to write. The shader still discards the color the same way. So for every draw with the
 * Z test on, Z write off and stencil off, declaring EarlyFragmentTests is free and exact, and the GPU stops
 * shading what is hidden.
 *
 * Deliberately left out: opaque alpha-tested draws (vegetation) write Z, so they are not touched; and
 * neither are draws inside a game occlusion query, because testing earlier would change the samples it
 * counts, and the game reads them.
 *
 * The "C6 early Z" report says how many draws are fixed and how many cannot be because they write
 * depth: that second figure is the exact size of what only a depth pre-pass would solve.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_early_z, true, "BO2",
                    "Native renderer: in draws that test depth but do NOT write it (smoke, particles, glass, "
                    "decals), declare EarlyFragmentTests in the pixel shader so the GPU tests depth BEFORE shading "
                    "instead of after. Without this, any shader with an alpha test or kill shades all its fragments "
                    "even when they end up hidden. The image is identical: there is no Z write to move earlier");
REXCVAR_DEFINE_INT32(bo2zm_native_early_z_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates the early depth test every N seconds, to "
                     "measure it A/B in the SAME session");
/*
 * The sky is drawn first and shades the whole screen for nothing.
 *
 * Measured, not estimated (FRAGMENT_SHADER_INVOCATIONS counter per draw): the sky dome is a single draw per
 * frame, 480 indices = 160 triangles, and yet it shades most of the screen with a 5-sample pixel shader. It
 * only acts on the dome of the shader kHashSky names, which no BO2 shader has.
 *
 * Why. The game draws it before the world. At that point the Z-buffer is empty, so nothing can discard it:
 * it shades the whole screen and then the world paints over it.
 *
 * Why deferring it does not change a single pixel. The draw is opaque (ONE/ZERO blending: it does not read
 * the target) and already tests depth with LEQUAL without writing it (RB_DEPTHCONTROL 00700732). So without
 * deferral, wherever the world covers it, the sky's result is overwritten; deferred, in the same place, the
 * depth test discards it before shading. The final color is the same in both cases, pixel for pixel, and
 * depth is not touched before or after because the sky does not write it.
 *
 * Where it is emitted. In the same pass, as soon as a draw arrives that it cannot be moved past (one with
 * blending, which does read the background color, or an opaque one that does not write depth, which the
 * sky would cover because the Z-buffer would not have changed), or when the pass closes, whichever comes
 * first. Every copy or resolve of the target goes through FinishPass (bo2zm_native_targets.cpp), so it
 * is emitted there too before anything reads the color.
 *
 * The only thing that could show, and how to recognize it. If the game drew something at exactly the same
 * depth as the dome (far plane z), the LEQUAL test would let the sky through and cover it, where without
 * deferral it would be the other way round. Nothing should be there (the sky is the farthest thing in the
 * scene), but if the sky is ever seen eating very distant geometry, this is why: disable the cvar.
 */
/*
 * Why the dome has to be identified as a draw, not as a shader.
 *
 * The sky's pixel shader is not used by the dome alone: in some scenes several draws per frame share it
 * (the "C6 postponed sky" line counts them). Deferring one of them breaks the order of the others, which
 * shows as flickering, badly rendered geometry and odd colors in the
 * menus and on dynamic models. The shader's fingerprint cannot tell those draws apart, so the dome is
 * identified by its index count (480 = 160 triangles), by being the first draw of the pass, and by
 * checking that it really covers the screen.
 *
 * On top of that sits a self-checking guard: even with the cvar on, nothing is deferred until 90
 * consecutive frames have been seen with a single dome, and as soon as two show up it switches off for the
 * rest of the run. See kSkyFramesTest and CloseGuardSkyFrame. Enabling it cannot break the image; at worst
 * it does nothing.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_postponed_sky, true, "BO2",
                    "Native renderer: postpone the sky draw until after the opaque draws of the same pass, "
                    "instead of drawing it first on an empty Z-buffer. It is a single opaque draw that tests depth and "
                    "does not write it, so the image is identical: what is overwritten today, tomorrow the depth test "
                    "discards");
/*
 * Draws that paint nothing and are still shaded.
 *
 * Two exact cases, both read from the draw's own registers:
 *
 *  1. Blending is "0 x source + 1 x destination" (ADD) on every written channel: the result is the
 *     destination as is. The draw cannot change a single color pixel.
 *  2. The alpha test function is 0 (NEVER): the recompiled shader always calls clip() (alphaTestValue
 *     returns -1 for case 0), so every fragment dies, and dies before writing depth.
 *
 * In both cases, if the draw also writes neither depth nor stencil, it leaves no trace of any kind and can
 * be skipped entirely. If it writes depth, only its color write is removed (mask 0), which already saves
 * the blending and the write.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_skip_invisible, true, "BO2",
                    "Native renderer: skip the draws that cannot change a single pixel (a blend that copies the "
                    "target, or an alpha test with the NEVER function). The image is identical by definition");
/*
 * Small per-draw savings on the ring thread. Each has its own cvar; the first three have a self-checking
 * guard (the first 200,000 cases, then 1 in 4,096, also go through the usual path and are compared; on a
 * difference, MISMATCH in the log and the saving is switched off).
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_framing_cache, true, "BO2",
                    "Native renderer: a draw's viewport, ndc and scissor are reused while neither "
                    "the framing registers (their generation) nor the pass change. Checks itself. false = computed for "
                    "every draw, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_useful_height_memo, true, "BO2",
                    "Native renderer: the target's useful height is noted in the map element of the "
                    "last pitch, without looking it up for every draw. Checks itself. false = as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_fast_pass_key, true, "BO2",
                    "Native renderer: if a draw's targets are the same bytes as the open pass's, "
                    "their XXH3 is not recomputed. Checks itself. false = XXH3 on every draw");
REXCVAR_DEFINE_BOOL(bo2zm_native_cvars_per_frame, true, "BO2",
                    "Native renderer: bo2zm_native_ps_alpha_only and "
                    "bo2zm_native_no_ps_without_color (and their alternations) are read once per frame and not on every "
                    "colorless draw. false = as before");
REXCVAR_DEFINE_INT32(bo2zm_native_no_ps_without_color_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates every N seconds building the pipeline "
                     "with and without a fragment stage in colorless draws, to compare captures of the same place with "
                     "the game paused");
REXCVAR_DEFINE_BOOL(bo2zm_native_no_ps_without_color, true, "BO2",
                    "Native renderer: in draws that write no color, builds the pipeline without a "
                    "fragment stage if the pixel shader can neither discard pixels nor write depth. The image does not "
                    "change and the GPU does not shade (shadow map)");
REXCVAR_DEFINE_STRING(bo2zm_native_diag_skip_ps, "", "BO2",
                      "Native renderer: comma-separated PS numbers whose draws are not recorded (tests only, to find a "
                      "draw)");
// In NVK for Tegra the memory type the SDK picks for uploads (without HOST_CACHED) is an NvMap without CPU
// caching, and writing there is slow on the console (other Switch projects measured the same). All vertex
// copies go to this buffer, so its memory type can be chosen for an A/B test.
REXCVAR_DEFINE_INT32(bo2zm_native_upload_memory, 0, "BO2",
                     "Native renderer: upload buffer memory. 0 = the one the SDK chooses (on the Switch, without CPU "
                     "cache), 1 = with CPU cache (published with vkFlushMappedMemoryRanges before submitting), 2 = "
                     "without CPU cache")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_shared_constants_cache, true, "BO2",
                    "Native renderer: each draw's shared constants (488 bytes) in a small buffer "
                    "WITH CPU cache, separate from the upload buffer. Measured in 155: writing them uncached cost 2.0 "
                    "us per draw and cached 0.4. Only with constants by UBO")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Direct-mapped pipeline cache.
 *
 * PipelineOf costs 0.52-0.76 us per draw in gameplay ("C6 substages"), more than BindVertexBuffers. The
 * one-entry shortcut misses on ~1 in 3 draws (the same ones that then call BindPipeline), and each miss is
 * an XXH3 of 80 bytes, a division by libstdc++'s prime bucket count and 2-3 jumps to nodes scattered across
 * the heap, which with the ring streaming megabytes through the cache almost always miss all the way to
 * memory: ~1.3 us per miss.
 *
 * In front of the map sits a table of 256 contiguous slots (22 KB) indexed by the low bits of the same
 * XXH3, holding the full key: a hit is one memcmp on a single slot. It only stores key -> VkPipeline pairs
 * already in the map, and the map never erases, so it cannot return anything other than what the map
 * would. Guard: the first 200,000 hits, then 1 in 4,096, also look up the map and must get the same
 * VkPipeline; on a difference, MISMATCH in the log and it switches off for the rest of the run. false = map
 * only.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_direct_pipelines, true, "BO2",
                    "Native renderer: a direct-mapped cache of 256 pipelines in front of "
                    "PipelineOf's map. Same result as the map (checks itself). false = only the map, as before");
/*
 * Measurement only, it changes no draw. What changes at each vkCmdBindPipeline of the ring: shaders, vertex
 * input, formats, specialization, or only fixed pipeline state (blending, masks, Z, stencil, cull face,
 * topology, bias, primitive restart). The state-only ones are those Vulkan dynamic state would avoid. Two
 * "C6 pipeline changes" lines every 20 s. false = nothing is counted.
 */
// Disabled by default: it was only a measurement and it has already produced its data.
REXCVAR_DEFINE_BOOL(bo2zm_native_count_pipeline_changes, false, "BO2",
                    "Native renderer: counts what changes at each vkCmdBindPipeline (measurement "
                    "only, changes no draw). C6 pipeline changes lines every 20 s. false = not counted");
/*
 * Dynamic state, phase 0a. A draw's pipeline is looked up with its key in canonical form (Canonicalize):
 * whatever PipelineOf does not read, or reads but Vulkan ignores (the blend equation without blendEnable,
 * the half of the equation whose channels the mask does not write, the Z function without a Z test, stencil
 * operations without stencil, targets not in the pass), is set to a fixed value. The pipeline is the same in
 * everything Vulkan looks at: fewer pipelines and fewer vkCmdBindPipeline calls (the counter's "no effect"
 * category). Guard: the first 200,000 key changes, then 1 in 4,096, compare the fixed state of both keys
 * field by field (FillStateFixed, the relevant part of PipelineOf); on a difference, MISMATCH in the
 * log and it switches off for the rest of the run. false = the usual key.
 */
// Off by default. Measured: PipelineOf gets more expensive and no avoidable bind was found.
REXCVAR_DEFINE_BOOL(bo2zm_native_canonical_key, false, "BO2",
                    "Native renderer: the pipeline is looked up with the key in canonical form (only "
                    "what Vulkan looks at): fewer pipelines and fewer vkCmdBindPipeline. Checks itself. false = the "
                    "usual key");
/*
 * Dynamic state, phase 0b. BeginPass does not forget the bound pipeline. Vulkan keeps the binding and
 * the dynamic state across passes of the same command buffer, and NVK only marks state as dirty when a pass
 * begins (nvk_cmd_buffer_dirty_render_pass). The same key carries the same formats and therefore a
 * compatible render pass (compatibility ignores loadOp and storeOp). A new command buffer still starts with
 * nothing bound. It removes the repeated vkCmdBindPipeline of a pass's first draw (the counter's "same key
 * after starting a pass"). false = forgotten at every pass.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_pipeline_across_passes, true, "BO2",
                    "Native renderer: the bound pipeline is kept when a pass of the same command "
                    "buffer begins (Vulkan keeps it). false = bound again in every pass, as before");
/*
 * Dynamic state, phase 1 (EDS1/EDS2, core in Vulkan 1.3; the console reports API 1.3.354). Cull mode, front
 * face, topology (within its class), Z test, write and function, stencil with its operations, depth bias
 * and primitive restart leave the pipeline: they are set with vkCmdSet* and only when they change. Two draws
 * that only differ in those share a pipeline and need no new vkCmdBindPipeline (3-4.5 us each on the
 * console). Pipelines in this mode carry a flag in the key (padding2) and do not mix with the usual ones;
 * the mode is decided once per command buffer. Guard: the first 200,000 key changes, then 1 in 4,096,
 * compare what was set with what the usual pipeline would carry (FillStateFixed); on a difference,
 * MISMATCH in the log and it switches off for the rest of the run (pipelines with all state baked in come
 * back, and the draw with the difference already goes out with its own). false = everything in the pipeline.
 */
// Off by default. Measured: each BindPipeline goes from 2.95 to 4.36 us and PipelineOf from 0.21 to 0.47 us per
// draw, for only 13 % fewer binds: a net loss of ~1 ms of ring time per frame.
REXCVAR_DEFINE_BOOL(bo2zm_native_dynamic_state, false, "BO2",
                    "Native renderer: cull mode, front face, topology, Z, stencil, bias and restart "
                    "with vkCmdSet* (Vulkan 1.3 EDS1/EDS2) instead of in the pipeline: fewer vkCmdBindPipeline. Checks "
                    "itself. false = everything in the pipeline, as before");
/*
 * Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3; NVK exposes it on Maxwell and the SDK enables it).
 * Blending (blendEnable), its equation and each target's color mask are set with vkCmdSet* and stop
 * splitting pipelines: among the first 64 pipelines created, the 12 state-only variants differed only in
 * blending. If the SDK or the device does not provide it, it is not used, and one log line says so. Same
 * guard as phase 1. false = blending in the pipeline.
 */
// Off by default, for the same measured reason as bo2zm_native_dynamic_state (it goes with it).
REXCVAR_DEFINE_BOOL(bo2zm_native_dynamic_state3, false, "BO2",
                    "Native renderer: blend, equation and color mask with vkCmdSet* "
                    "(VK_EXT_extended_dynamic_state3) instead of in the pipeline. Checks itself. false = in the "
                    "pipeline, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_upload_memory, true, "BO2",
                    "Native renderer: when the upload buffer is created, measures once how many MB/s the CPU writes "
                    "into each visible memory type (8 MB) and how much publishing them costs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in native/bo2zm_render_stubs.cpp (a stub for BO2), which owns the lever.
namespace bo2zm::guard30 {
bool NoShadows(bool of_user);
}  // namespace bo2zm::guard30

namespace bo2zm::native {

// Incremented by the ring thread when it handles a WAIT_REG_MEM (bo2zm_native_render.cpp). That is the
// point where vertex deduplication must forget what it recorded.
std::atomic<uint32_t> g_syncs_ring{0};
std::atomic<bool> g_syncs_on_flush_waits{false};
namespace {

/*
 * A pass's category from its render target, in one place.
 *
 * The width alone cannot tell the shadow map from the scene ("1600 or more" would count a 1920-wide scene
 * as shadows), so the shadow map is recognized for what it is: a depth-only target, with no color at all.
 */
inline uint32_t TargetCategory(uint32_t pitch, const uint64_t* keys) {
  const bool color = keys[0] || keys[1] || keys[2] || keys[3];
  if (!color && keys[4] && pitch >= 1600) {
    return kGpuShadows;
  }
  if (pitch >= 1280) {
    return keys[4] ? kGpuScene : kGpuSceneNoDepth;
  }
  if (pitch >= 640) {
    return kGpuReflection;
  }
  if (pitch >= 320) {
    return kGpu320;
  }
  return kGpuSmaller;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;

constexpr uint32_t kRegConstantsVs = 0x4000;
constexpr uint32_t kRegConstantsPs = 0x4400;
constexpr uint32_t kRegFetch = 0x4800;
constexpr uint32_t kRegBooleans = 0x4900;
constexpr uint32_t kRegistersConstants = 0x400;  // 256 constants x 4

// 16384 2D slots. A full texture cache holds ~4,800-5,000 textures in a long match, and a texture without
// a slot would be drawn with slot 0, the empty texture, cleared to transparent black: black floors and zombies in Town
// with 4096 slots, more so deep into a match and on the lower presets (smaller textures, more of them in the same
// megabytes). The shaders index unbounded arrays with a whole word (tools/xenosrecomp/translator/shader_common.h), so
// only this needs to change.
constexpr uint32_t kCapacityHeap[4] = {16384, 64, 256, 512};  // 2D, 3D, cube, samplers
// Work slots. The ones actually used are chosen by bo2zm_native_work_slots; this is the room reserved
// for them, and it has to match the array in bo2zm_native_targets.cpp.
constexpr size_t kSlotsOfJob = 3;
constexpr VkDeviceSize kSizeUpload = VkDeviceSize(64) << 20;
// Separate buffer for the shared constants, per slot. ~1,400 draws x 512 bytes = 0.7 MB per frame; if it
// fills up, the work is submitted just as with the upload buffer.
constexpr VkDeviceSize kSizeShared = VkDeviceSize(4) << 20;  // per work slot (there are three)
// The pipeline cache and the prewarm list go in a single file in <NRO folder>/cache/ (nothing loose next to
// the NRO, and a single pipelines .bin). Header "BZPC", version, list bytes and cache bytes (two uint32
// and two uint64), then both parts: the list as is (its "BZPL" header and the records) and the
// vkGetPipelineCacheData data.
// A build for another program of the game, which shares the folder, keeps its pipelines in a folder of its own.
#ifndef BO2ZM_NATIVE_CACHE_FOLDER
#define BO2ZM_NATIVE_CACHE_FOLDER "cache"
#endif
constexpr const char* kFolderCache = BO2ZM_NATIVE_CACHE_FOLDER;
constexpr const char* kFilePipelines = "bo2zm_native_pipelines.bin";
constexpr uint32_t kMagicFilePipelines = 0x43505A42u;  // "BZPC" in little-endian
constexpr uint32_t kVersionFilePipelines = 1;
constexpr size_t kHeaderFilePipelines = 2 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
// The two files of the older layout, next to the NRO: if the single file does not exist yet they are read
// once (so an existing cache is not lost) and deleted when the single file is written.
constexpr const char* kFileCacheOld = "bo2zm_native_pipelines.bin";
constexpr const char* kFileListOld = "bo2zm_native_pipelines_list.bin";
// The pipeline prewarm list. It holds no game data: state keys, formats and fingerprints. A header of four
// uint32 ("BZPL", version, record size and record count) followed by the records (PipelineRecord).
constexpr uint32_t kMagicListPipelines = 0x4C505A42u;  // "BZPL" in little-endian
constexpr uint32_t kVersionListPipelines = 1;
constexpr size_t kHeaderList = 4 * sizeof(uint32_t);
constexpr size_t kMaxRegistersList = 4096;
// 296 bytes: shader_common.h (g_NdcScale at +280 and g_NdcOffset at +288).
// Shared constants: texture and sampler indices (0-63), booleans, texcoords, half pixel, alpha threshold
// (68) and function (69), NDC (64-73) and g_InputRemap for the 16 locations (74-89).
// 90 words up to g_InputRemap (bytes 296..359) and 32 more for 1/size of the 16 texture slots (bytes
// 360..487), which avoid querying the texture size on every sample.
// + 16 words for the fetch scale of each slot (bytes 488-551; see WriteScale), 2 of padding, and
// 32 for the texture coordinate scale of each slot (bytes 560-687; see WriteScaleCoords).
// + 8 words with the iteration count of the 32 loop constants (0x4908-0x4927; one byte each, loop i in word
// 172 + i / 4): BO2's two looping pixel shaders take theirs from the draw, not from a literal (bytes 688-719).
// + the vertex shaders' texture slots (fetch constants 16-19, CTAB samplers 0-3; slot k): texture indices
// 180 + dimension * 4 + k, sampler 192 + k, 1/size 196 + k * 2, fetch scale 204 + k, coordinate scale 208 + k * 2
// (bytes 720-863). Keep in step with tools/xenosrecomp/translator/shader_recompiler.cpp.
constexpr uint32_t kWordsShared = 216;
constexpr uint32_t kWordScale = 122;
constexpr uint32_t kWordScaleCoords = 140;
constexpr uint32_t kWordInvSize = 90;
constexpr uint32_t kWordLoops = 172;
constexpr uint32_t kWordsVsSlots = 180;
constexpr uint32_t kSlotsPs = 16;  // slots 16-19 are the vertex shader's
constexpr uint32_t kSlotsVs = 4;
constexpr uint32_t WordTexture(uint32_t heap, uint32_t slot) {
  return slot < kSlotsPs ? heap * 16 + slot : kWordsVsSlots + heap * 4 + (slot - kSlotsPs);
}
constexpr uint32_t WordSampler(uint32_t slot) { return slot < kSlotsPs ? 48 + slot : 192 + (slot - kSlotsPs); }
constexpr uint32_t WordInvSize(uint32_t slot) {
  return slot < kSlotsPs ? kWordInvSize + slot * 2 : 196 + (slot - kSlotsPs) * 2;
}
constexpr uint32_t WordScale(uint32_t slot) { return slot < kSlotsPs ? kWordScale + slot : 204 + (slot - kSlotsPs); }
constexpr uint32_t WordScaleCoords(uint32_t slot) {
  return slot < kSlotsPs ? kWordScaleCoords + slot * 2 : 208 + (slot - kSlotsPs) * 2;
}
// Constants through a dynamic UBO (bo2zm_native_constants_ubo). The bit is SPEC_CONSTANT_CONSTANTS_UBO
// from shader_common.h, and the sizes are the blocks the shaders declare: 256 and 224 float4, and 23 shared
// float4.
constexpr uint32_t kSpecConstantsUbo = uint32_t(1) << 8;
// Internal bit of the pipeline key that no shader reads: the bright pass (the container kHashBrightPass
// names) uses the bo2zm_sky_glow variant: natural
// (bo2zm_native_glow_energy_spirv.h) or soft (bo2zm_native_smooth_glow_spirv.h), when those are built.
// The shaders take 1/texture size from the shared constants (SPEC_CONSTANT_INV_TEX_SIZE in XenosRecomp)
// and do not query the texture size on every sample.
constexpr uint32_t kSpecInvSizeTex = uint32_t(1) << 9;
constexpr uint32_t kSpecGlowNatural = uint32_t(1) << 12;
constexpr uint32_t kSpecGlowSmooth = uint32_t(1) << 13;
// The pixel shader is compiled without its color writes (pass without a color target).
constexpr uint32_t kSpecOnlyAlpha = uint32_t(1) << 14;
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets (nine samples
// per pixel, more in some). With this bit
// the eight outer offsets are set to zero, the nine samples become identical and the compiler merges them
// into one. Shadow edges lose some smoothness. Needs the shader library regenerated.
constexpr uint32_t kSpecPcfCheap = uint32_t(1) << 15;
// Bits 16-18 = the alpha test function (0-6). See SPEC_CONSTANT_ALPHA_FUNC_SHIFT in shader_common.h: it
// removes the 7-case switch from every pixel shader.
constexpr uint32_t kSpecFunctionAlphaOffset = 16;
// The radial blur of the final composite (see SPEC_CONSTANT_NO_BLUR).
constexpr uint32_t kSpecNoBlur = uint32_t(1) << 19;
// Internal bit of the pipeline key that no shader reads (shader_common.h goes up to bit 19). It marks that
// the pixel shader module is the copy with OpExecutionMode EarlyFragmentTests.
constexpr uint32_t kSpecZEarly = uint32_t(1) << 20;
// bo2zm_native_shadow_min. SPEC_CONSTANT_SHADOW_MIN from shader_common.h: tfetch2DShadowMin takes the
// minimum of the shadow map and its pair, which goes in the 3D index word of that register.
constexpr uint32_t kSpecShadowMin = uint32_t(1) << 23;
constexpr uint64_t kHashComposite = 0x19C0C358044A29BFull;  // a final composite; no BO2 shader has this hash
// bo2zm_visual_treatment. The final composite (kHashComposite) tints each channel with a polynomial
// curve (Coeffs0..3 = c6..c9, evaluated at MISCMAP1.w), mixes in a desaturated part with its x component and adds a
// vignette (VisualEffectVignette.x = c1). With a neutral curve (Coeffs0 = (0, 1, 1, 1) and Coeffs1..3 = 0) and the
// vignette at 0 the picture has no filter; the glow (g_fBloomScale, c4) and the brightness of the fades
// (CombinedBrightness, c10) stay the same. soft: halfway between the game's values and the neutral ones (the curve is
// linear in its coefficients). The registers are those of the microcode, the same in every edition.
constexpr uint32_t kBytesTreatment = 11 * 16;  // c0 to c10
void ApplyTreatmentVisual(float* c, int mode) {
  static constexpr float kNeutral[4][4] = {{0, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  const float f = mode == 2 ? 1.0f : 0.5f;
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i < 4; ++i) {
      float& v = c[(6 + k) * 4 + i];
      v += (kNeutral[k][i] - v) * f;
    }
  }
  c[1 * 4 + 0] *= 1.0f - f;
}

/*
 * The render target height comes from the pitch, not from what the game uses.
 *
 * bo2zm_native_targets.cpp creates every render target with `height = max(720, pitch)`, because the real
 * height is not known when it is created. For example:
 *
 *   scene    pitch 880 -> image 880x880, BO2 uses 880x720
 *   narrow targets (pitch 256 or less) -> 2048 rows (HeightTarget)
 *
 * And since the passes are opened with loadOp = LOAD and closed with storeOp = STORE, that whole area is
 * read and written in every pass opening of the frame: many megabytes per frame moved for nothing, over a
 * 21.3 GB/s bus that the three cores also share.
 *
 * The Xbox 360 paid none of this: its framebuffer lived in 10 MB of on-chip EDRAM at 256 GB/s, and the
 * resolve was a hardware operation. We move it back and forth.
 *
 * It is fixed through the renderArea, not the image size. Vulkan only loads and stores the renderArea;
 * everything outside is preserved. So the height does not have to be guessed when the image is created:
 * opening the pass over the rectangle the game really uses is enough.
 *
 * How that height is known without guessing: from the draws' scissor. The maximum seen per pitch is kept,
 * only grows, and is rounded up to a multiple of 64. Until there is data the whole pass is opened, so the
 * first frame never clips too much.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_useful_area_pass, true, "BO2",
                    "Native renderer: open each pass on the rectangle the game really uses instead of on the "
                    "whole image. The targets' height is deduced from the pitch, so the scene opens 880x880 to draw "
                    "880x720. The image is identical: Vulkan only loads and stores the renderArea");
// A bright pass (bo2zm_sky_glow); no BO2 shader has this hash.
constexpr uint64_t kHashBrightPass = 0xE849A9F6D3323B87ull;
/*
 * The pixel shader of the sky dome (bo2zm_native_postponed_sky).
 *
 * The fingerprint is the XXH3 of the shader's original container, not of the translated SPIR-V: it
 * identifies the game's shader and does not change when the library is regenerated (two library builds whose
 * SPIR-V for it differed gave the same fingerprint). It is the same mechanism as kHashBrightPass and
 * kHashComposite. None of the three hashes is in BO2's shader set, so those paths do nothing in BO2.
 *
 * What it is, beyond doubt: its constant table declares CloudIntensity, SkyAlphaTag and Brightness, and
 * four samplers (DIFFUSEMAP, MISCMAP1, MISCMAP2, MISCMAP3) with 5 samples, and it was the only shader in its
 * library that names the sky: it cannot be confused with any other.
 */
constexpr uint64_t kHashSky = 0x28AA3CDAC6C19705ull;
// The sky dome is 480 indices = 160 triangles, measured in the log (the C6 diag line of the dome's draw).
// That is the mark that tells the dome draw apart from the others that share its pixel shader, which are
// the ones that would break the image if they were deferred.
constexpr uint32_t kIndicesDomeSky = 480;
/*
 * How long the sky guard's test lasts, counting only the frames in which the dome appears (menus and
 * loading screens do not count, see CloseGuardSkyFrame). 90 frames are about 3 seconds
 * of play: enough not to decide on four samples, and short enough for the saving to start almost as
 * play begins. All 90 must have exactly one dome: as soon as two appear in the same frame (the
 * earlier failure) it switches off for good.
 */
constexpr uint32_t kSkyFramesTest = 90;
constexpr uint32_t kSkyFramesWithOne = 90;
constexpr VkDeviceSize kUboBytesVs = 256 * 16;
// 256, as the library's Bo2zmBlockPs declares (BO2 reads pixel constants up to c250).
constexpr VkDeviceSize kUboBytesPs = 256 * 16;
constexpr VkDeviceSize kUboBytesShared = 54 * 16;  // kWordsShared / 4; the library declares 54
constexpr uint32_t kRemapIdentity = 0xFFF;
constexpr uint32_t kMaxVerticesPerDraw = uint32_t(1) << 20;
constexpr uint32_t kMemoryPhysical = 0x20000000;
// Times a statement in the timed draws (C6 substages).
#define BO2ZM_SUB(k, ...) \
  do { \
    if (time_) { \
      const auto t0_sub_ = std::chrono::steady_clock::now(); \
      __VA_ARGS__; \
      sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                 std::chrono::steady_clock::now() - t0_sub_) \
                                 .count()); \
      ++sub_n_[k]; \
    } else { \
      __VA_ARGS__; \
    } \
  } while (0)

/*
 * From 8 to 64. Each timed draw reads the clock about 20 times (Stage, BO2ZM_SUB, samplers and
 * vertices). At 1 in 8 that averaged 0.15-0.4 us on every draw of the ring, and in the alley there are
 * 2,500-4,000 per frame. "C6 stages", "C6 substages" and "ms copying vertices" divide (or rescale) by
 * the draws that actually carried a stopwatch, so their scale does not change: they just have 8 times
 * fewer samples (~600-1,400 per second in gameplay).
 */
constexpr uint32_t kStopwatchEvery = 64;  // draws per stage-timed draw (power of 2)
constexpr size_t kCopiesPerWarning = 64;   // copies queued between wake-ups of the copy thread (power of 2)
// Copies taken in one batch (bo2zm_native_upload_help). In gameplay they are ~2 KB each: a batch is
// ~30 KB, about 20 us. That is how long the ring may have to wait for the thread, already with its
// priority lent.
constexpr size_t kCopiesPerChunk = 16;
// Waits with pending copies in the observing phase (the ring only waits and checks the marks) before it
// starts helping. In the menus that is a few seconds.
constexpr uint64_t kWaitsCopiesObserving = 512;

// One guest vertex binding into the upload buffer, as host words with the fetch constant's byte order.
// Done by the ring thread or by the copy thread (bo2zm_native_upload_thread).
/*
 * A change check for bo2zm_native_vertex_cache, cheaper than XXH3 on the A57. XXH3_64bits multiplies in
 * its inner loop and hashes ~2 GB/s on the console (3.2 GB per 10 s took 1.6 s of the ring thread, ~16 % of a core).
 * These are Fletcher sums of the 16-byte blocks in 64-bit lanes (s1 += block, s2 += s1), four independent
 * pairs for the four blocks of each 64 bytes so the adds overlap, then XXH3 of the sixteen lanes and the length.
 * Position-sensitive (s2 weighs each block by its place; the four pairs keep its place within 64 bytes) and with no
 * multiply in the loop. Like XXH3 here it only answers "did these bytes change"; the vertex cache's guard still
 * compares the bytes of a sample of the hits.
 */
uint64_t ChangeSum(const uint8_t* data, size_t bytes) {
  uint64_t lanes[17];
#if defined(__aarch64__)
  uint64x2_t a1 = vdupq_n_u64(0), a2 = vdupq_n_u64(0), b1 = a1, b2 = a1, c1 = a1, c2 = a1, d1 = a1, d2 = a1;
  size_t i = 0;
  for (; i + 64 <= bytes; i += 64) {
    a1 = vaddq_u64(a1, vreinterpretq_u64_u8(vld1q_u8(data + i)));
    b1 = vaddq_u64(b1, vreinterpretq_u64_u8(vld1q_u8(data + i + 16)));
    c1 = vaddq_u64(c1, vreinterpretq_u64_u8(vld1q_u8(data + i + 32)));
    d1 = vaddq_u64(d1, vreinterpretq_u64_u8(vld1q_u8(data + i + 48)));
    a2 = vaddq_u64(a2, a1);
    b2 = vaddq_u64(b2, b1);
    c2 = vaddq_u64(c2, c1);
    d2 = vaddq_u64(d2, d1);
  }
  uint8_t tail[64] = {};
  std::memcpy(tail, data + i, bytes - i);
  a1 = vaddq_u64(a1, vreinterpretq_u64_u8(vld1q_u8(tail)));
  b1 = vaddq_u64(b1, vreinterpretq_u64_u8(vld1q_u8(tail + 16)));
  c1 = vaddq_u64(c1, vreinterpretq_u64_u8(vld1q_u8(tail + 32)));
  d1 = vaddq_u64(d1, vreinterpretq_u64_u8(vld1q_u8(tail + 48)));
  a2 = vaddq_u64(a2, a1);
  b2 = vaddq_u64(b2, b1);
  c2 = vaddq_u64(c2, c1);
  d2 = vaddq_u64(d2, d1);
  vst1q_u64(lanes + 0, a1);
  vst1q_u64(lanes + 2, a2);
  vst1q_u64(lanes + 4, b1);
  vst1q_u64(lanes + 6, b2);
  vst1q_u64(lanes + 8, c1);
  vst1q_u64(lanes + 10, c2);
  vst1q_u64(lanes + 12, d1);
  vst1q_u64(lanes + 14, d2);
#else
  uint64_t s1[8] = {}, s2[8] = {};
  size_t i = 0;
  auto block = [&](const uint8_t* p) {
    for (int k = 0; k < 8; ++k) {
      uint64_t w;
      std::memcpy(&w, p + 8 * k, 8);
      s1[k] += w;
      s2[k] += s1[k];
    }
  };
  for (; i + 64 <= bytes; i += 64) {
    block(data + i);
  }
  uint8_t tail[64] = {};
  std::memcpy(tail, data + i, bytes - i);
  block(tail);
  for (int k = 0; k < 8; ++k) {
    lanes[2 * k] = s1[k];
    lanes[2 * k + 1] = s2[k];
  }
#endif
  lanes[16] = bytes;
  return XXH3_64bits(lanes, sizeof(lanes));
}

struct CopyJob {
  const uint8_t* source = nullptr;
  uint8_t* target = nullptr;
  uint32_t words = 0;
  xenos::Endian order = xenos::Endian::kNone;
};

// The plain copy: the loop GCC vectorizes 16 bytes at a time.
void CopyVerticesPlain(const uint8_t* const source, uint8_t* const target, const uint32_t words,
                       const xenos::Endian order) {
  if (order == xenos::Endian::k8in32) {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  } else {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = xenos::GpuSwap(v, order);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  }
}

#if defined(__aarch64__)
constexpr size_t kCopyPrefetchAhead = 512;

// bo2zm_native_copy_neon: 64 bytes a step (ld1/st1 of four vectors), the source asked for kCopyPrefetchAhead bytes
// ahead (PRFM PLDL1STRM), the first lines at once. kOrder is the xenos::Endian value.
template <int kOrder>
void CopyVerticesNeon(const uint8_t* source, uint8_t* target, uint32_t words) {
  auto swap = [](uint8x16_t v) {
    if constexpr (kOrder == int(xenos::Endian::k8in16)) {
      return vrev16q_u8(v);
    } else if constexpr (kOrder == int(xenos::Endian::k8in32)) {
      return vrev32q_u8(v);
    } else if constexpr (kOrder == int(xenos::Endian::k16in32)) {
      return vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    } else {
      return v;
    }
  };
  const size_t bytes = size_t(words) * 4;
  for (size_t ahead = 64; ahead < kCopyPrefetchAhead && ahead < bytes; ahead += 64) {
    __builtin_prefetch(source + ahead, 0, 0);
  }
  size_t i = 0;
  for (; i + 64 <= bytes; i += 64) {
    if (i + kCopyPrefetchAhead < bytes) {
      __builtin_prefetch(source + i + kCopyPrefetchAhead, 0, 0);
    }
    uint8x16x4_t v = vld1q_u8_x4(source + i);
    v.val[0] = swap(v.val[0]);
    v.val[1] = swap(v.val[1]);
    v.val[2] = swap(v.val[2]);
    v.val[3] = swap(v.val[3]);
    vst1q_u8_x4(target + i, v);
  }
  for (; i + 16 <= bytes; i += 16) {
    vst1q_u8(target + i, swap(vld1q_u8(source + i)));
  }
  for (; i < bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, source + i, 4);
    v = xenos::GpuSwap(v, xenos::Endian(kOrder));
    std::memcpy(target + i, &v, 4);
  }
}

// The NEON copy against the plain one: every byte order, sizes around the step lengths, misaligned ends.
bool CopyVerticesNeonSelfTest() {
  std::vector<uint8_t> source(9000), expected(9000), got(9000);
  uint32_t seed = 12345;
  for (auto& b : source) {
    seed = seed * 1664525u + 1013904223u;
    b = uint8_t(seed >> 24);
  }
  for (int order = 0; order < 4; ++order) {
    for (uint32_t words : {0u, 1u, 3u, 4u, 15u, 16u, 17u, 31u, 33u, 127u, 128u, 129u, 1000u, 2047u}) {
      for (size_t offset : {size_t(0), size_t(4), size_t(12)}) {
        std::fill(expected.begin(), expected.end(), uint8_t(0xA5));
        std::fill(got.begin(), got.end(), uint8_t(0xA5));
        CopyVerticesPlain(source.data() + offset, expected.data() + offset, words, xenos::Endian(order));
        switch (order) {
          case 1:
            CopyVerticesNeon<1>(source.data() + offset, got.data() + offset, words);
            break;
          case 2:
            CopyVerticesNeon<2>(source.data() + offset, got.data() + offset, words);
            break;
          case 3:
            CopyVerticesNeon<3>(source.data() + offset, got.data() + offset, words);
            break;
          default:
            CopyVerticesNeon<0>(source.data() + offset, got.data() + offset, words);
            break;
        }
        if (got != expected) {
          REXLOG_ERROR("[native] C6: NEON vertex copy differs from the plain loop (order {}, {} words, offset {}): "
                       "the plain loop stays",
                       order, words, offset);
          return false;
        }
      }
    }
  }
  return true;
}
#endif

// bo2zm_native_copy_neon, and its A/B factor.
bool CopyNeonOn() {
#if defined(__aarch64__)
  static const bool on = [] {
    if (!REXCVAR_GET(bo2zm_native_copy_neon)) {
      return false;
    }
    const bool ok = CopyVerticesNeonSelfTest();
    if (ok) {
      REXLOG_INFO("[native] C6: vertex copies in 64-byte NEON steps, {} bytes prefetched ahead (checked against the "
                  "plain loop)",
                  kCopyPrefetchAhead);
    }
    return ok;
  }();
  static const int factor = REXCVAR_GET(bo2zm_native_copy_neon_ab);
  if (on && (factor == 1 || factor == 2)) {
    const int arm = bo2zm::benchmark::AbArm();
    return arm < 0 || (arm & factor) != 0;
  }
  return on;
#else
  return false;
#endif
}

void CopyVertices(const CopyJob& t) {
  // The fields go into local variables: the destination is a byte pointer and, as far as the compiler
  // knows, writing through it could change the structure itself. With t.* inside the loop it was not
  // vectorized (935 ms per 5.3 GB of vertices in gameplay, against 490 ms with the loop inside Draw).
  const uint8_t* const source = t.source;
  uint8_t* const target = t.target;
  const uint32_t words = t.words;
  const xenos::Endian order = t.order;
#if defined(__aarch64__)
  if (CopyNeonOn()) {
    switch (order) {
      case xenos::Endian::k8in16:
        CopyVerticesNeon<1>(source, target, words);
        return;
      case xenos::Endian::k8in32:
        CopyVerticesNeon<2>(source, target, words);
        return;
      case xenos::Endian::k16in32:
        CopyVerticesNeon<3>(source, target, words);
        return;
      default:
        CopyVerticesNeon<0>(source, target, words);
        return;
    }
  }
#endif
  CopyVerticesPlain(source, target, words, order);
}

// Allocator that leaves whatever resize adds uninitialized: indices_, converted_ and indices16_ are
// fully rewritten right after, and std::vector's zero fill was 1.4 % of the ring thread on PC.
template <class T>
struct UninitializedAllocator : std::allocator<T> {
  using value_type = T;
  UninitializedAllocator() = default;
  template <class U>
  UninitializedAllocator(const UninitializedAllocator<U>&) noexcept {}
  template <class U>
  struct rebind {
    using other = UninitializedAllocator<U>;
  };
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

using FnBufferAddress = VkDeviceAddress(VKAPI_PTR*)(VkDevice, const VkBufferDeviceAddressInfo*);

float Float(uint32_t value) {
  return std::bit_cast<float>(value);
}

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
uint32_t Log2Ceiling(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

uint32_t Log2Floor(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

/*
 * The sample used to recheck a stable texture (bo2zm_native_hash_sampling). From a region of guest
 * memory, its first 4 KB block, its last one and one in every kSampleEvery are read, counted from the
 * start of the region. The base and the mips of a texture start at 4 KB-aligned addresses, so each block
 * is a whole page. That reads ~13-19 % of the bytes of textures of 64 KB or more; for textures of few
 * blocks the sample is almost everything, and PrepareTexture does not use it if it exceeds half. Tested
 * on PC: BytesSample matches what is read for every size from 1 byte to 300 KB, and a one-byte change is
 * seen if and only if it falls in a sampled block.
 */
constexpr uint64_t kBlockSample = 4096;
constexpr uint64_t kSampleEvery = 8;

// Bytes HashSample reads in a region of that size.
inline uint64_t BytesSample(uint64_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  return (last + kSampleEvery - 1) / kSampleEvery * kBlockSample + (bytes - last * kBlockSample);
}

// Chained XXH3 of blocks 0, 8, 16... below the last one, and of the last one (which may be partial).
inline uint64_t HashSample(const uint8_t* data, uint64_t bytes, uint64_t seed) {
  if (!bytes) {
    return seed;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  uint64_t hash = seed;
  for (uint64_t b = 0; b < last; b += kSampleEvery) {
    hash = XXH3_64bits_withSeed(data + b * kBlockSample, size_t(kBlockSample), hash);
  }
  return XXH3_64bits_withSeed(data + last * kBlockSample, size_t(bytes - last * kBlockSample), hash);
}

// First level of the packed tail: once the short side is 16 texels or less.
uint32_t LevelPacked(uint32_t width, uint32_t height) {
  const uint32_t l = Log2Ceiling(std::min(width, height));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
void OffsetPacked(uint32_t width, uint32_t height, uint32_t block, uint32_t level, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_width = Log2Ceiling(width);
  const uint32_t l2_height = Log2Ceiling(height);
  const uint32_t l2 = std::min(l2_width, l2_height);
  x = 0;
  y = 0;
  if (l2 > 4 + level) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = level - base;
  if (m < 3) {
    if (l2_width > l2_height) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_width > l2_height) {
    x = (1u << (l2_width - base)) >> (m - 2);
  } else {
    y = (1u << (l2_height - base)) >> (m - 2);
  }
  x /= block;
  y /= block;
}

// Address of a block in a texture tiled in 32x32 blocks, copied from GetTiledOffset2D
// (graphics/pipeline/texture/util.cpp). pitch in blocks.
int32_t OffsetTiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Fast untiling. During gameplay stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): ReadLevel called OffsetTiled2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as OffsetTiled2D (checked block by block on PC).
template <uint32_t kLog2>
void UntileLevel(const uint8_t* source, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* target) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGroup = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_row = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t row = 0; row < by; ++row) {
    const int32_t y = int32_t(oy + row);
    const int32_t macro_y = (y >> 5) * macros_row;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* output = target + size_t(row) * bx_host * kBytes;
    uint32_t column = 0;
    while (column < bx) {
      const int32_t x = int32_t(ox + column);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t displacement = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGroup > 1 && (uint32_t(x) % kGroup) == 0 && column + kGroup <= bx) {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kGroup * kBytes);
        column += kGroup;
      } else {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kBytes);
        ++column;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void ChangeOrderBytes(uint8_t* data, size_t bytes, uint32_t unit, uint32_t order) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // xenos::Endian
  size_t n = unit == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unit == 2 && order != k8in16) || (unit != 2 && unit != 4) || order == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(data + i);
    uint8x16_t r;
    if (unit == 2 || order == k8in16) {
      r = vrev16q_u8(v);
    } else if (order == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(data + i, r);
  }
#endif
  if (unit == 2 || order == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(data[i], data[i + 1]);
    }
  } else if (order == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (order == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }
}

// The same for a 3D texture tiled in 32x32x4 blocks, copied from GetTiledOffset3D
// (graphics/pipeline/texture/util.cpp:438-459). pitch and height in blocks.
int32_t OffsetTiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  height = (height + 31) & ~uint32_t(31);
  const int32_t macro_exterior = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_exterior) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t exterior = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = exterior + ((((x >> 3) + (exterior << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// XenosRecomp's USAGE_LOCATIONS (shader_recompiler.cpp).
int32_t UseLocation(uint8_t use, uint8_t index) {
  switch (use) {
    case 0:  // position
      return index == 0 ? 0 : (index == 1 ? 15 : -1);
    case 3:  // normal
      return index == 0 ? 1 : -1;
    case 6:  // tangent
      return index == 0 ? 2 : -1;
    case 7:  // binormal
      return index == 0 ? 3 : -1;
    case 5:  // texcoord
      return index < 4 ? 4 + index : (index < 8 ? 12 + (index - 4) : -1);
    case 10:  // color
      return index == 0 ? 8 : (index == 1 ? 11 : -1);
    case 2:  // blend indices
      return index == 0 ? 9 : -1;
    case 1:  // blend weights
      return index == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// USAGE_TYPES: these inputs are uint4 in the translated shaders.
bool InputWhole(uint8_t use) {
  // Only BLENDINDICES. Since the library build that validated normals, normals, tangents and binormals are
  // float4: the game stores them as 16-bit integers (format 26) or as floats, and with uint4 the shader's
  // asfloat gave degenerate directions (seen in reflections).
  return use == 2;
}

uint32_t ComponentsVertex(uint32_t format) {
  switch (format) {
    case 33:
    case 36:
      return 1;
    case 25:
    case 31:
    case 34:
    case 37:
      return 2;
    case 16:
    case 17:
    case 57:
      return 3;
    default:
      return 4;
  }
}

// Components written by a fetch swizzle (the ones that are not 7), as a 4-bit mask.
uint32_t MaskWritten(uint32_t swizzle) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mask |= 1u << i;
    }
  }
  return mask;
}

// g_InputRemap code: the SPIR-V writes r[i] = input[orig[i]] and the fetch patched by D3D writes
// r[i] = data[patched[i]] (or 0 / 1). For each written component, the host input at orig[i] has to
// come from patched[i]. 7 = the same component.
uint32_t CodeRemap(uint32_t original, uint32_t patched) {
  uint32_t code = kRemapIdentity;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (patched >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      code = (code & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return code;
}

VkFormat FormatAttribute(uint32_t format, bool input_whole, bool with_sign, bool whole,
                         bool red_blue, bool& r11g11b10) {
  r11g11b10 = false;
  const auto choose = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled,
                          VkFormat uint_, VkFormat sint) {
    if (input_whole) {
      return with_sign ? sint : uint_;
    }
    if (whole) {
      return with_sign ? sscaled : uscaled;
    }
    return with_sign ? snorm : unorm;
  };
  switch (format) {
    case 6:  // k_8_8_8_8
      return red_blue ? choose(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM,
                                VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_B8G8R8A8_SSCALED,
                                VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_B8G8R8A8_SINT)
                       : choose(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                                VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:  // k_2_10_10_10
      if (red_blue) break;
      return choose(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                    VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                    VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 16:  // k_10_11_11: packed normal decoded by the shader itself
      if (!input_whole || red_blue) break;
      r11g11b10 = true;
      return VK_FORMAT_R32_UINT;
    case 25:  // k_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                    VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:  // k_16_16_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                    VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                    VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:  // k_16_16_FLOAT
      return input_whole || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return input_whole || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:  // k_32
      return input_whole && !red_blue ? (with_sign ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT)
                                         : VK_FORMAT_UNDEFINED;
    case 34:  // k_32_32
      return input_whole && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT)
                 : VK_FORMAT_UNDEFINED;
    case 35:  // k_32_32_32_32
      return input_whole && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                 : VK_FORMAT_UNDEFINED;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (input_whole ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT);
    case 37:  // k_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (input_whole ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT);
    case 57:  // k_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (input_whole ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT);
    case 38:  // k_32_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (input_whole ? VK_FORMAT_R32G32B32A32_UINT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT);
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);

struct HostTextureFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint8_t block = 1;       // texels per block side
  uint8_t bytes = 1;        // bytes per block
  uint8_t unit_order = 0;  // 0 = no byte swap, else 2 or 4 bytes
  uint16_t swizzle_host = kSwizzleRGBA;
};

// Same host formats as the emulation (vulkan/texture_cache.cpp:123-397).
bool FormatTextureOf(uint32_t format, HostTextureFormat& f) {
  switch (format) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {VK_FORMAT_R8_UNORM, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:  // k_8_8
      f = {VK_FORMAT_R8G8_UNORM, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:  // k_DXN
      f = {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 24:  // k_16
      f = {VK_FORMAT_R16_UNORM, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:  // k_16_16
      f = {VK_FORMAT_R16G16_UNORM, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {VK_FORMAT_R16_SFLOAT, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {VK_FORMAT_R16G16_SFLOAT, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {VK_FORMAT_R32_SFLOAT, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {VK_FORMAT_R32G32_SFLOAT, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

bool IsDepth(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// GuestToHostSwizzle: the fetch constant's swizzle, mapped through the channels the host format has.
VkComponentMapping MappingComponents(uint32_t swizzle_guest, uint16_t swizzle_host) {
  VkComponentMapping mapping{};
  VkComponentSwizzle* output[4] = {&mapping.r, &mapping.g, &mapping.b, &mapping.a};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t value = (swizzle_guest >> (i * 3)) & 0x7;
    if (value <= 3) {
      const uint32_t host = (swizzle_host >> (value * 3)) & 0x7;
      *output[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + host);
    } else if (value == 4) {
      *output[i] = VK_COMPONENT_SWIZZLE_ZERO;
    } else {
      *output[i] = VK_COMPONENT_SWIZZLE_ONE;
    }
  }
  return mapping;
}

struct VertexAttribute {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t offset;
};

struct VertexBinding {
  uint32_t slot;   // vertex fetch constant (0-95)
  uint32_t stride;  // bytes
};

struct VertexInput {
  std::vector<VertexAttribute> attributes;
  std::vector<VertexBinding> bindings;
  std::array<uint32_t, 16> remaps{};  // g_InputRemap by location
  uint32_t specialization = 0;
  uint64_t hash = 0;
};

// Copy of the SPIR-V without the writes to the color targets (output variables with Location 0..3).
// Returns empty if the module cannot be walked or there was nothing to remove.
std::vector<uint32_t> PruneColorWrites(const std::vector<uint32_t>& spirv, uint32_t& removed) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpDecorate = 71, kOpVariable = 59, kOpStore = 62;
  constexpr uint32_t kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kDecorationBuiltIn = 11, kDecorationLocation = 30, kDecorationIndex = 29;
  constexpr uint32_t kStoreOutput = 3;
  removed = 0;
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    return {};
  }
  std::unordered_set<uint32_t> with_location, discarded;
  // 1) decorations: Location 0..3 marks a color target; BuiltIn or Index != 0 rule it out (gl_FragDepth
  // and the second blend source are not touched).
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      return {};
    }
    if (code == kOpDecorate && words >= 3) {
      const uint32_t goal = spirv[i + 1];
      const uint32_t decoration = spirv[i + 2];
      if (decoration == kDecorationLocation && words >= 4 && spirv[i + 3] <= 3) {
        with_location.insert(goal);
      } else if (decoration == kDecorationBuiltIn ||
                 (decoration == kDecorationIndex && words >= 4 && spirv[i + 3] != 0)) {
        discarded.insert(goal);
      }
    }
    i += words;
  }
  // 2) pointers to those targets: the output variable and whatever is derived from it through
  // OpAccessChain.
  std::unordered_set<uint32_t> pointers;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (code == kOpVariable && words >= 4 && spirv[i + 3] == kStoreOutput) {
      const uint32_t id = spirv[i + 2];
      if (with_location.count(id) && !discarded.count(id)) {
        pointers.insert(id);
      }
    } else if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && words >= 4 &&
               pointers.count(spirv[i + 3])) {
      pointers.insert(spirv[i + 2]);
    }
    i += words;
  }
  if (pointers.empty()) {
    return {};
  }
  // 3) the writes to those pointers are dropped. The rest of the module is copied as is: the output
  // variable stays declared and in the entry point's interface, which is valid even if it is never written.
  std::vector<uint32_t> output;
  output.reserve(spirv.size());
  output.insert(output.end(), spirv.begin(), spirv.begin() + 5);
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    const bool outside = code == kOpStore && words >= 3 && pointers.count(spirv[i + 1]);
    if (outside) {
      ++removed;
    } else {
      output.insert(output.end(), spirv.begin() + i, spirv.begin() + i + words);
    }
    i += words;
  }
  (void)kOpEntryPoint;
  return removed ? output : std::vector<uint32_t>();
}

/*
 * The same module, but declaring OpExecutionMode EarlyFragmentTests.
 *
 * In SPIR-V the execution modes have their own section, right after the OpEntryPoint instructions and
 * before the debug strings and decorations. So it is enough to insert the instruction after the module's
 * last OpExecutionMode (there is always at least one, OriginUpperLeft, which is what DXC emits). No new
 * ids are created, so the header's "bound" does not change and the module stays valid.
 *
 * Returns empty (and then the normal module is used) if the module has no fragment entry point, if it
 * already declared it, or if it writes gl_FragDepth (DepthReplacing): in that last case testing earlier
 * would change the result, because the Z being tested is computed by the shader itself.
 */
std::vector<uint32_t> WithTestsEarly(const std::vector<uint32_t>& spirv, const char*& reason) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16;
  constexpr uint32_t kModelFragment = 4;
  constexpr uint32_t kModeEarly = 9, kModeDepthReplacing = 12;
  reason = "";
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    reason = "not SPIR-V";
    return {};
  }
  uint32_t input = 0;
  size_t where = 0;  // first word after the execution mode section
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      reason = "malformed module";
      return {};
    }
    if (code == kOpEntryPoint && words >= 3) {
      if (spirv[i + 1] == kModelFragment) {
        if (input) {
          reason = "more than one fragment entry point";
          return {};
        }
        input = spirv[i + 2];
      }
      if (where < i + words) {
        where = i + words;
      }
    } else if (code == kOpExecutionMode && words >= 3) {
      if (spirv[i + 2] == kModeEarly) {
        reason = "already declared it";
        return {};
      }
      if (spirv[i + 2] == kModeDepthReplacing) {
        reason = "writes gl_FragDepth";
        return {};
      }
      if (where < i + words) {
        where = i + words;
      }
    }
    i += words;
  }
  if (!input || !where) {
    reason = "no fragment entry point";
    return {};
  }
  std::vector<uint32_t> output;
  output.reserve(spirv.size() + 3);
  output.insert(output.end(), spirv.begin(), spirv.begin() + where);
  output.push_back((3u << 16) | kOpExecutionMode);
  output.push_back(input);
  output.push_back(kModeEarly);
  output.insert(output.end(), spirv.begin() + where, spirv.end());
  return output;
}

struct PipelineKey {
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint64_t input = 0;
  uint32_t topology = 0;
  uint32_t specialization = 0;
  uint32_t formats[5] = {};
  uint32_t blend[4] = {};
  uint32_t masks = 0;
  uint32_t depth = 0;
  uint32_t rasterization = 0;
  // The key is hashed and compared byte by byte (PipelineOf). With 76 bytes of fields and 8-byte alignment
  // it had 4 bytes of uninitialized implicit padding: stack garbage that made identical keys differ and
  // created duplicate pipelines (125-134, and 193-203 in a later version, with no change in the image).
  uint32_t padding = 0;
  uint32_t padding2 = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineKey>,
              "PipelineKey cannot have implicit padding: it is hashed and compared byte by byte");

/*
 * One pipeline of the prewarm list (bo2zm_native_prewarm_pipelines).
 *
 * What is needed to recreate it in a later launch exactly as the ring created it: its key as PipelineOf
 * looks it up; the fingerprint of its two shaders in the library (if the library has changed, key.vs
 * and key.ps are no longer the same shaders and the record is skipped); and its vertex input, which the
 * key only carries as a fingerprint. It is stored on disk byte for byte, so it cannot have implicit
 * padding.
 */
struct RegisterAttribute {
  uint32_t location = 0;
  uint32_t binding = 0;
  uint32_t format = 0;  // VkFormat
  uint32_t offset = 0;
};

struct PipelineRecord {
  static constexpr uint32_t kMaxAttributes = 16;
  static constexpr uint32_t kMaxBindings = 16;
  PipelineKey key;
  uint64_t hash_vs = 0;  // bo2zm::shaderlib::Shader::hash
  uint64_t hash_ps = 0;  // 0 without a fragment stage (key.ps == 0)
  uint32_t n_attributes = 0;
  uint32_t n_bindings = 0;
  RegisterAttribute attributes[kMaxAttributes] = {};
  uint32_t strides[kMaxBindings] = {};
};
static_assert(std::has_unique_object_representations_v<PipelineRecord>,
              "PipelineRecord is saved to disk byte by byte: no implicit padding");

/*
 * bo2pso text lists (docs/mac/pso-list-format.md, bo2zm_native_pso.h). A line describes the pipeline in the
 * portable fields of the format and carries the renderer's own record in the state's x:rec extra (hex), which is
 * what the loader feeds back into the prewarm list: the native renderer creates pipelines from PipelineRecord, and
 * its shader hashes (hash_vs, hash_ps) are checked against the library before a record is compiled, so a list from
 * another shader pack only costs the records whose shaders are gone.
 */
const char* PsoTopology(uint32_t topology) {
  switch (VkPrimitiveTopology(topology)) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return "point_list";
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return "line_list";
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return "line_strip";
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return "triangle_list";
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return "triangle_strip";
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return "triangle_fan";
    default: return "other";
  }
}

std::string PsoFormat(uint32_t format) {
  switch (VkFormat(format)) {
    case VK_FORMAT_UNDEFINED: return "-";
    case VK_FORMAT_R8G8B8A8_UNORM: return "rgba8";
    case VK_FORMAT_B8G8R8A8_UNORM: return "bgra8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "rgba8srgb";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "rgb10a2";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "rgba16f";
    case VK_FORMAT_R16G16_SFLOAT: return "rg16f";
    case VK_FORMAT_R16G16B16A16_UNORM: return "rgba16";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "rg11b10f";
    case VK_FORMAT_R32_SFLOAT: return "r32f";
    case VK_FORMAT_R32G32_SFLOAT: return "rg32f";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "rgba32f";
    case VK_FORMAT_D16_UNORM: return "d16";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "d24s8";
    case VK_FORMAT_D32_SFLOAT: return "d32f";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "d32fs8";
    default: return fmt::format("vk{}", format);
  }
}

// The line without its uses field, and its key (XXH3 of that text).
std::string PsoLine(const PipelineRecord& r, uint64_t& key_line) {
  const PipelineKey& k = r.key;
  std::string state = fmt::format("fmt={}.{}.{}.{},zfmt={},samples=1,blend=", PsoFormat(k.formats[0]),
                                  PsoFormat(k.formats[1]), PsoFormat(k.formats[2]), PsoFormat(k.formats[3]),
                                  PsoFormat(k.formats[4]));
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t m = k.blend[i];
    state += fmt::format("{}{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}", i ? "/" : "", m & 0x1F, (m >> 8) & 0x1F,
                         (m >> 5) & 0x7, (m >> 16) & 0x1F, (m >> 24) & 0x1F, (m >> 21) & 0x7,
                         (k.masks >> (i * 4)) & 0xF);
  }
  const uint32_t d = k.formats[4] ? k.depth : 0;
  state += fmt::format(",depth={}.{}.{}", (d >> 1) & 1, ((d >> 1) & 1) && ((d >> 2) & 1), (d >> 4) & 7);
  if (d & 1) {
    state += fmt::format(",stencil={:x}", d);  // the packed RB_DEPTHCONTROL stencil fields (front and back)
  } else {
    state += ",stencil=0";
  }
  const uint32_t ras = k.rasterization;
  state += fmt::format(",cull={},front={},fill=solid,bias={},vtx={:016x},alpha2c=0,x:spec={:x},x:restart={}",
                       (ras & 0x3) == 0x3 ? "both" : (ras & 0x1) ? "front" : (ras & 0x2) ? "back" : "none",
                       (ras & 0x4) ? "cw" : "ccw", (ras & 0x10) ? 1 : 0, k.input, k.specialization,
                       (ras & 0x8) ? 1 : 0);
  state += ",x:rec=";
  static constexpr char kHex[] = "0123456789abcdef";
  const auto* bytes = reinterpret_cast<const uint8_t*>(&r);
  for (size_t i = 0; i < sizeof(r); ++i) {
    state += kHex[bytes[i] >> 4];
    state += kHex[bytes[i] & 0xF];
  }
  const std::string ps = k.ps ? fmt::format("{:016x}", r.hash_ps) : std::string("-");
  const std::string body = fmt::format("{:016x}\t{}\t{}\t{}", r.hash_vs, ps, PsoTopology(k.topology), state);
  key_line = XXH3_64bits(body.data(), body.size());
  return fmt::format("G\t{:016x}\t{}", key_line, body);
}

// The record in a line's x:rec extra; false if the line has none or it does not fit this build's record.
bool PsoRecordOf(std::string_view line, PipelineRecord& r) {
  const size_t at = line.find("x:rec=");
  if (at == std::string_view::npos) {
    return false;
  }
  std::string_view hex = line.substr(at + 6);
  hex = hex.substr(0, hex.find_first_of(",\t\r"));
  if (hex.size() != sizeof(PipelineRecord) * 2) {
    return false;
  }
  auto* bytes = reinterpret_cast<uint8_t*>(&r);
  const auto nibble = [](char c) -> int {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  for (size_t i = 0; i < sizeof(r); ++i) {
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    bytes[i] = uint8_t(hi << 4 | lo);
  }
  return r.n_attributes <= PipelineRecord::kMaxAttributes && r.n_bindings <= PipelineRecord::kMaxBindings;
}

// A tiled 3D texture kept up to date by 4 KB pages (VulkanDrawsImpl::UpdateVolume). The game
// rewrites a few hundred bytes of its model lighting volume (512x256x4, 2 MB) almost every frame; reading, untiling,
// byte-swapping, hashing and uploading all of it each time would cost the console's ring thread ~8-11 ms per
// frame.
struct IncrementalVolume {
  uint32_t address = 0;  // guest physical address
  uint32_t bound = 0;    // guest bytes it spans by the tiling's bound (ReadVolume's check: may overshoot)
  uint32_t pages = 0;    // 4 KB pages that really hold its texels (BO2's volume: 512, 2 MB; the bound said 4.1 MB,
                           // and the pages past 2 MB are other game data that changes every frame)
  std::vector<uint8_t> raw;  // the guest bytes as last read
  std::vector<uint64_t> hashes;  // per page with texels: XXH3 of its bytes in raw (what was untiled)
  std::vector<uint8_t> host;   // the untiled, byte-swapped base level: what the image holds
  // A run of consecutive x in one row: contiguous in guest memory (16 bytes at most).
  struct Span {
    uint32_t source;   // offset in raw
    uint32_t target;  // offset in host
    uint16_t bytes;
    uint16_t x;
    uint16_t y;
    uint16_t z;
  };
  std::vector<Span> spans;
  std::vector<uint32_t> start_page;      // runs of page p: spans_page[start_page[p] .. start_page[p + 1])
  std::vector<uint32_t> spans_page;
  uint32_t row = 0;  // host bytes per row
  uint32_t slice = 0;  // host bytes per slice
  uint32_t height = 0;   // host rows per slice
  uint32_t depth3d = 0;
  uint32_t width_host = 0;
  uint32_t unit_order = 0;
  uint32_t order = 0;
};

struct CachedTexture {
  NativeImage image;
  uint32_t layers = 1;  // 6 for cubemaps
  uint32_t depth3d = 0;  // slices of 3D textures; 0 for the rest
  uint64_t hash = 0;
  uint64_t frame = UINT64_MAX;
  uint64_t staging_failed_frame = UINT64_MAX;  // do not expose an uninitialized image after a failed upload
  uint64_t hash_raw = 0;  // XXH3 of the guest bytes (2D and cubemaps)
  uint64_t next = 0;     // frame of the next check
  uint32_t interval = 1;     // frames between checks: 1 to 32
  uint32_t postponements = 0;  // consecutive checks deferred by the budget
  // Sample fingerprint (HashSample) of the same content as hash_raw if sample_valid, and how many
  // consecutive rechecks have been accepted on it alone (bo2zm_native_hash_sampling).
  uint64_t hash_sample = 0;
  bool sample_valid = false;
  uint8_t samples_consecutive = 0;
  bool needs_upload = false;
  std::vector<uint8_t> data;  // levels already laid out for the host: level after level and, in each, layer after layer
  uint32_t levels = 1;        // mip levels of the host image
  std::array<uint32_t, 16> offset_level{};  // data bytes up to each level
  uint64_t bytes = 0;          // what it counts in bytes_textures_ (for eviction)
  // 1 + its index in in_flight_ while the bind thread runs its vkBindImageMemory; 0 = no. While set, the
  // image exists but has no memory: no command may reference it until CollectBindings.
  uint32_t in_flight = 0;
  // Its data and hash_raw are prepared by the hash thread (bo2zm_native_texture_hash_thread). 0 = no;
  // 1 + its index in hashes_planned_ until UploadTexture; kJobHashPublished until CollectHashes
  // sets its hash_raw. While nonzero it is not evicted and its hash_raw is not valid yet.
  uint32_t hash_job = 0;
  // Measurement only (bo2zm_native_diag_reuse): its shape without the address (when the image is
  // created), its address (only for the log), the content key it is filed under in live_per_content_
  // (0 = none) and whether it is new and its first fingerprint has not been measured yet.
  uint64_t shape_content = 0;
  uint64_t key_content = 0;
  uint32_t address = 0;
  bool content_per_measure = false;
  // bo2zm_native_texture_by_writes: the physical ranges its check reads (base and mips), the stamp epoch
  // of its last check (0 = none), its rechecks skipped since the last guard check, and whether this check is a guard.
  uint32_t stamp_base = 0, stamp_base_bytes = 0, stamp_mips = 0, stamp_mips_bytes = 0;
  uint32_t check_epoch = 0;
  uint16_t stamp_skips = 0;
  bool guard_stamps = false;
  // 3D textures updated by pages (UpdateVolume) and the regions of a partial upload (data then
  // holds only those rows, back to back; bufferOffset is relative to data).
  std::unique_ptr<IncrementalVolume> incremental;
  std::vector<VkBufferImageCopy> regions_partial;
};

struct View {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t slot = 0;
  uint32_t heap = 0;  // 0 2D textures, 2 cubemaps
};

class VulkanDrawsImpl final : public VulkanDraws {
 public:
  VulkanDrawsImpl(const VulkanDevice* vulkan_device, rex::memory::Memory* guest_memory,
                    TargetContext* context)
      : vulkan_device_(vulkan_device),
        dfn_(deferred::Table(*vulkan_device)),  // the device's, with the vkCmd* calls queued (deferred.h)
        gpu_(dfn_),
        device_(vulkan_device->device()),
        memory_(guest_memory),
        context_(context) {}

  ~VulkanDrawsImpl() override {
    StopPrewarm();  // uses the pipeline cache and the layout: first
    StopCopies();
    StopBindings();  // in-flight vkBindImageMemory calls finish before any image is destroyed
    StopHashes();  // the hash thread writes to the upload buffer: join it before releasing that
    SaveCachePipelines();
    StopWriterCache();  // writes whatever is still pending
    ClosePsoRecord();
    for (auto& [key, pair] : pipelines_) {
      dfn_.vkDestroyPipeline(device_, pair.second, nullptr);
    }
    if (cache_pipelines_ != VK_NULL_HANDLE) {
      destroy_cache_(device_, cache_pipelines_, nullptr);
    }
    for (auto& [key, framebuffer] : framebuffers_) {
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (VkFramebuffer framebuffer : fb_retired_) {  // the ones ForgetView retired
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (auto& [key, pass] : passes_) {
      dfn_.vkDestroyRenderPass(device_, pass, nullptr);
    }
    for (auto& [input, shader_module] : modules_) {
      dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
    }
    for (auto& [input, shader_module] : modules_only_alpha_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (auto& [input, shader_module] : modules_z_early_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (VkShaderModule shader_module : modules_variants_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (auto& [key, view] : views_) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
    }
    for (auto& [key, texture] : textures_) {
      DestroyImage(texture.image);
    }
    for (auto& [key, pair] : samplers_) {
      dfn_.vkDestroySampler(device_, pair.first, nullptr);
    }
    for (NativeImage& empty : empty_) {
      DestroyImage(empty);
    }
    // The pool's slabs are released after destroying every image that lives in them. The other way round
    // would free memory that the VkImages still have bound.
    pool_textures_.Finish();
    if (sampler_empty_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_empty_, nullptr);
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    bo2zm::gpu_skin::SetRendererReady(false);
    if (skin_pipeline_ != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, skin_pipeline_, nullptr);
    if (skin_layout_ != VK_NULL_HANDLE) dfn_.vkDestroyPipelineLayout(device_, skin_layout_, nullptr);
    if (skin_table_buffer_ != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, skin_table_buffer_, nullptr);
    if (skin_table_memory_ != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, skin_table_memory_, nullptr);
    if (skin_pool_buffer_ != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, skin_pool_buffer_, nullptr);
    if (skin_pool_memory_ != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, skin_pool_memory_, nullptr);
    if (pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pool_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_ubo_, nullptr);
    if (layout_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout_ubo_, nullptr);
    for (VkDescriptorSetLayout layout : layouts_) {
      if (layout != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    for (const UploadBuffer& s : shared_bufs_) {
      if (s.data) dfn_.vkUnmapMemory(device_, s.guest_memory);
      if (s.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buffer, nullptr);
      if (s.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.guest_memory, nullptr);
    }
    for (const UploadBuffer& s : uploads_) {
      if (s.data) dfn_.vkUnmapMemory(device_, s.guest_memory);
      if (s.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buffer, nullptr);
      if (s.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.guest_memory, nullptr);
    }
    for (const VertexPool& v : vpools_) {  // bo2zm_native_vertex_cache
      if (v.data) dfn_.vkUnmapMemory(device_, v.memory);
      if (v.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, v.buffer, nullptr);
      if (v.memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, v.memory, nullptr);
    }
  }

  bool Init() {
    reuse_textures_on_ = REXCVAR_GET(bo2zm_native_reuse_textures);  // texture words reuse (Draw)
    dedupe_keep_flush_ = REXCVAR_GET(bo2zm_native_dedupe_keep_on_flush_waits);
    g_syncs_on_flush_waits.store(!dedupe_keep_flush_, std::memory_order_relaxed);
    const auto& properties = vulkan_device_->properties();
    const std::pair<bool, const char*> requirements[] = {
        {properties.shaderInt64, "shaderInt64"},
        {properties.bufferDeviceAddress, "bufferDeviceAddress"},
        {properties.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {properties.shaderSampledImageArrayDynamicIndexing,
         "shaderSampledImageArrayDynamicIndexing"},
        {properties.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {properties.descriptorBindingSampledImageUpdateAfterBind,
         "descriptorBindingSampledImageUpdateAfterBind"},
        {properties.descriptorBindingUpdateUnusedWhilePending,
         "descriptorBindingUpdateUnusedWhilePending"},
    };
    for (const auto& [present, label] : requirements) {
      if (!present) {
        REXLOG_ERROR("[native] C6: the Vulkan device does not have {}: nothing is drawn", label);
        return false;
      }
    }
    address_buffer_ = reinterpret_cast<FnBufferAddress>(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkGetBufferDeviceAddress"));
    gpu_.loaded().copy_image = reinterpret_cast<PFN_vkCmdCopyImage>(deferred::Wrap(
        "vkCmdCopyImage",
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdCopyImage")));
    LoadCachePipelines();
    LoadStateDynamic();  // dynamic state phases 1 and 2
    if (!address_buffer_ || !CreateUpload() || !CreateDescriptors()) {
      return false;
    }
    CreateGpuSkin();  // bo2zm_gpu_skin
    // The pool is created after CreateDescriptors (where textures_mb_max_ is read) and before CreateEmpty,
    // so the three empty images can already come from it. The slabs are prewarmed here, while the game is
    // still loading: a 32 MB memset during gameplay would be a 10-15 ms stutter.
    pool_textures_.Start(vulkan_device_, textures_mb_max_);
    return CreateEmpty();
  }

  bool Draw(const DrawRequest& p) override {
    VerdictVegetation(p);  // the ring's versus the game's (bo2zm_d3d_game_vegetation)
    // Stage stopwatch on 1 in kStopwatchEvery draws: reading the clock 12 times per draw took almost a
    // quarter of the ring thread's busy CPU on PC, and on the Switch each read costs more. The C6 averages
    // come from the sample.
    time_ = (++stopwatch_counter_ & (kStopwatchEvery - 1)) == 0;
    auto mark = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint32_t* r = p.registers;
    if (!r || !p.vs) {
      return false;
    }
    // --- Primitive type ---------------------------------------------------
    const uint32_t mode_edram = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
    if (mode_edram != uint32_t(xenos::EdramMode::kColorDepth) && mode_edram != 5) {
      return Reject(1, "EDRAM mode without color or depth");
    }
    // Mode 5 (depth only): the Xenos does not run the pixel shader (IsPixelShaderNeededWithRasterization,
    // graphics/util/draw.cpp:125-129). Without a PS there are no textures, pixel constants or alpha test,
    // and the pipeline has no fragment stage. The shadow draws work this way, and some arrive with the PS
    // object set to 0.
    const ShaderInput* ps =  // can be removed if it writes no color and does not discard
        mode_edram == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
    if (!ps && mode_edram == uint32_t(xenos::EdramMode::kColorDepth)) {
      return false;
    }
    // Diagnostic: PS whose draws are skipped (bo2zm_native_diag_skip_ps).
    const std::string& skip = diag_skip_ps_text_;
    if (skip != skip_text_) {
      skip_text_ = skip;
      skip_ps_.clear();
      uint32_t number = 0;
      bool has = false;
      for (char c : skip + ",") {
        if (c >= '0' && c <= '9') {
          number = number * 10 + uint32_t(c - '0');
          has = true;
        } else if (has) {
          skip_ps_.insert(number);
          number = 0;
          has = false;
        }
      }
      if (!skip_ps_.empty()) {
        REXLOG_WARN("[native] C6 diag: PS {} are not drawn", skip_text_);
      }
    }
    if (ps && !skip_ps_.empty() && skip_ps_.count(ps->number)) {
      return true;
    }
    const uint32_t initiator = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
    const uint32_t type = initiator & 0x3F;
    const uint32_t source_kind = (initiator >> 6) & 0x3;
    const bool indices32 = (initiator >> 11) & 0x1;
    const uint32_t count = initiator >> 16;
    // This draw's index cache set and stamp line, fetched early (bo2zm_native_prefetch_cache_lines).
    if (prefetch_lines_ && icache_active_ && source_kind == uint32_t(xenos::SourceSelect::kDMA) && !indices32) {
      const uint32_t physical = r[gr::XE_GPU_REG_VGT_DMA_BASE] & 0x1FFFFFFEu;
      PrefetchCacheLines(physical | (uint64_t(count & 0x3FFFFFF) << 29) |
                             (uint64_t((r[gr::XE_GPU_REG_VGT_DMA_SIZE] >> 30) & 3) << 55) | (uint64_t(1) << 62) |
                             (uint64_t(1) << 63),
                         physical);
    }
    if (!count) {
      return true;
    }
    VkPrimitiveTopology topology;
    bool quads = false;
    bool accepts_restart = false;
    switch (type) {
      case 2:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        break;
      case 3:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        accepts_restart = true;
        break;
      case 4:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case 5:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        accepts_restart = true;
        break;
      case 6:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        accepts_restart = true;
        break;
      case 13:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        quads = true;
        break;
      default:
        return Reject(100 + type, "primitive type not supported yet");
    }
    if (source_kind == uint32_t(xenos::SourceSelect::kImmediate)) {
      return Reject(2, "immediate indices");
    }
    // Clipping disabled on the Xenos: the position may come in pixels (videos). It is drawn with a viewport
    // the size of the render target and the transform in the VS, like the emulation
    // (graphics/util/draw.cpp:341-363).
    const bool no_clip = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
    const uint32_t mode_sc = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    if (((mode_sc >> 3) & 0x3) == 2 && ((mode_sc >> 5) & 0x7) != 2) {
      return Reject(4, "polygons drawn as points or lines");
    }

    const VertexInput* input = InputOf(p);
    if (!input) {
      return false;
    }
    Stage(0, mark);
    std::chrono::steady_clock::time_point t_indices_185 = mark;  // C6 substages 15-18

    // --- Targets --------------------------------------------------------------
    const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
    const uint32_t mask_register =
        mode_edram == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
    static constexpr uint32_t kInfoColor[4] = {
        gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
        gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
    uint64_t keys[5] = {};
    uint32_t masks = 0;
    bool has_target = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t mask = (mask_register >> (i * 4)) & 0xF;
      if (!mask || !ps || !((ps->outputs >> i) & 0x1)) {
        continue;
      }
      const uint32_t info = r[kInfoColor[i]];
      const uint32_t format = (info >> 16) & 0xF;
      if (!bo2zm_formats::ColorApprox(format)) {
        return Reject(200 + format, "color target format not supported yet");
      }
      keys[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(format) << 16) |
                  pitch;
      masks |= mask << (i * 4);
      has_target = true;
    }
    /*
     * Draws that cannot change a single pixel (bo2zm_native_skip_invisible).
     *
     * Two cases, read from this draw's own registers without any heuristics:
     *
     *  - Blending is "0 x source + 1 x destination" (ADD) on the written channels: the result is the
     *    destination as is. It is checked per channel group because the mask may write only RGB or only
     *    alpha, and then the other group's factors do not matter.
     *  - The alpha test function is 0 = NEVER: alphaTestValue (shader_common.h) returns -1 and clip() kills
     *    every fragment, before depth is written.
     *
     * If there is also no depth or stencil left to write, the whole draw is unnecessary: it is dropped. If
     * it writes depth it is drawn anyway and only counted, because setting the color mask to 0 would send it
     * down the "no color" path (kSpecOnlyAlpha), where bo2zm_shadows_without_vegetation drops the whole draw and
     * its depth would be lost. Removing the color write saves too little to be worth it.
     * The pruning is skipped while an occlusion query is open: the game reads those samples.
     */
    if (skip_invisible_ && masks) {
      static constexpr uint32_t kBlendOf[4] = {
          gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
          gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
      const uint32_t control_color_inv = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool alpha_never =
          ((control_color_inv >> 3) & 0x1) && (control_color_inv & 0x7) == 0;  // function NEVER
      uint32_t masks_useful = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t mask_i = (masks >> (i * 4)) & 0xF;
        if (!mask_i) {
          continue;
        }
        const uint32_t m = r[kBlendOf[i]] & 0x1FFF1FFF;
        const bool copy_color = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
        const bool copy_alpha =
            ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
        const bool writes_rgb = (mask_i & 0x7) != 0 && !copy_color;
        const bool writes_alpha = (mask_i & 0x8) != 0 && !copy_alpha;
        if (writes_rgb || writes_alpha) {
          masks_useful |= mask_i << (i * 4);
        }
      }
      if ((!masks_useful || alpha_never) && !occlusion_open_) {
        // With the NEVER function even the depth write dies (clip() runs in the shader, before the merger),
        // so the draw leaves no trace of any kind. With the blending that copies the destination, the draw can
        // only be dropped if it writes neither Z nor stencil either.
        const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
        const bool leaves_trail =
            !alpha_never && ((((dc >> 1) & 0x1) && ((dc >> 2) & 0x1)) || (dc & 0x1));
        ++counts_z_[alpha_never ? kInvisibleAlpha : kInvisibleBlend];
        if (!leaves_trail) {
          return true;
        }
        ++counts_z_[kInvisibleOnlyColor];  // drawn anyway: only counts what is left to gain
      }
    }
    // With no color to write, the pixel shader can only have an effect by discarding pixels (alpha test or
    // its own kill) or by writing depth. If it does neither, the pipeline has no fragment stage and the image
    // is identical: the GPU saves shading the whole shadow map.
    // Of the draws that do write color, how many can use early depth rejection and how many force shading
    // before testing. That is what decides whether a depth pre-pass would help at all.
    if (masks && ps && has_target) {
      const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool test_alpha =
          ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
      const bool discards = test_alpha || ps->discards || (ps->outputs & 0x10);
      if (pitch >= 1600) {
        // shadow map: already counted separately
      } else if (discards) {
        ++scene_with_discard_;
      } else {
        ++scene_no_discard_;
      }
    }
    bool only_alpha = false;
    if (!masks && ps) {
      const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool test_alpha =
          ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
      if (!test_alpha && !ps->discards && !(ps->outputs & 0x10)) {
        ++draws_ps_useless_;
        if (cvars_per_frame_ ? no_ps_no_color_frame_ : NoPsNoColor()) {
          ps = nullptr;
        }
      } else {
        ++draws_ps_needed_;
        // Diagnostic: in the shadow map the pixel shader reads constant c1 (g_bShadowMapAlphaEnabled). With
        // that constant at 0 it neither samples nor discards: the stage would be unnecessary.
        if (pitch >= 1600) {
          (Float(r[kRegConstantsPs + 4]) != 0.0f ? ++shadows_alpha_active_ : ++shadows_alpha_off_);
        }
        // Needed for the alpha test or a kill, but its color goes nowhere: it is compiled without those
        // writes.
        if (cvars_per_frame_ ? ps_only_alpha_frame_ : PsOnlyAlpha()) {
          only_alpha = true;
        }
      }
    }
    /*
     * Shadow map vegetation discard, as early as possible.
     *
     * Discarding these draws right before the pipeline lookup would mean paying for them in full first:
     * index conversion, vertex sources, PrepareTexture for all their samplers, upload buffer allocation,
     * pass change, vertex copy, index and constant memcpy, viewport, scissor and pipeline lookup.
     *
     * And it is not a handful of draws: 604 per frame are dropped, 31 % of those that come in (the
     * `draws_ps_needed` counter matches the lost ones exactly across twelve gameplay intervals). At
     * ~8.5 us per incoming draw, that is 3-5 ms.
     *
     * Careful when measuring this: the divisor of `C6 stages` is the recorded draws (~1,370), not the
     * incoming ones (~1,970), even though the label says otherwise. Multiplying by the incoming ones inflates
     * the budget by 45 %.
     *
     * The occlusion guard is free and removes the only doubt: with a game occlusion query open the draw
     * counts even if it is not visible, so there it takes the long path and the usual discard drops it.
     */
    const bool vegetation_early = only_alpha && no_vegetation_ && !occlusion_open_;
    // The ring's verdict for the bo2zm_d3d_game_vegetation guard (VerdictVegetation) must be this same
    // decision. With the settings read on every draw (cvars_per_frame_ false) the alternation can change
    // between the two reads: then no comparison is made.
    if (vegetation_computed_ && cvars_per_frame_ && vegetation_ring_ != vegetation_early) {
      vegetation_detail_.early = vegetation_early;
      AnnotateVegetationModel(vegetation_flags_, vegetation_detail_);
    }
    if (vegetation_early) {
      ++draws_vegetation_soon_;
      return true;
    }
    const uint32_t control_depth = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
    if (control_depth & 0x3) {  // stencil o z
      const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
      keys[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) |
                  (uint64_t((info >> 16) & 0x1) << 16) | pitch;
      has_target = true;
    }
    if (!has_target || !pitch) {
      // Diagnostic: a draw inside an occlusion query that writes nothing counts no samples.
      if (occlusion_open_ && pitch >= 640 && warnings_occlusion_draw_ < 8) {
        ++warnings_occlusion_draw_;
        REXLOG_INFO("[native] C2 occlusion draw {}: NO TARGET (not drawn): VS n{} PS n{} type {} count {} pitch {} mask "
                    "{:08X} depth {:08X} EDRAM mode {}",
                    warnings_occlusion_draw_, p.vs->number, ps ? int(ps->number) : -1, type, count, pitch,
                    mask_register, control_depth, mode_edram);
      }
      return true;  // writes nothing visible
    }
    // FPS test bo2zm_native_skip_shadows: a depth-only target 1600 or more wide (BO2's 560-wide one is not). With
    // bo2zm_native_skip_shadows_alternate_s = N they are skipped in the odd N-second intervals.
    const bool target_shadows =  // also used for its depth bias
        pitch >= 1600 && keys[4] && !keys[0] && !keys[1] && !keys[2] && !keys[3];
    if (target_shadows) {
      // The 30 FPS guard removes the whole pass. Skipping it on 2 of every 3 frames instead would flicker,
      // so there is no such intermediate step.
      bool skip = bo2zm::guard30::NoShadows(REXCVAR_GET(bo2zm_native_skip_shadows));
      const int32_t alternate = REXCVAR_GET(bo2zm_native_skip_shadows_alternate_s);
      if (!skip && alternate > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_shadows_)
                                  .count();
        skip = (seconds / alternate) % 2 == 1;
        if (skip != shadows_skipped_) {
          shadows_skipped_ = skip;
          REXLOG_INFO("[native] shadow test: {} (frame {})",
                      skip ? "without shadows" : "with shadows", frame_);
        }
      }
      if (skip) {
        return true;
      }
    }

    CutSubstage(15, t_indices_185);  // render targets and discards
    // --- Vertex and index range -------------------------------------------------
    const uint32_t displacement = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
    const bool restart = accepts_restart && ((mode_sc >> 21) & 0x1);
    // Depth bias as in the emulation with host render targets (GetPreferredFacePolygonOffset,
    // graphics/util/draw.cpp:92-118).
    float scale_bias = 0.0f, offset_bias = 0.0f;
    if (type >= 4) {  // polygons
      if (((mode_sc >> 11) & 0x1) && !(mode_sc & 0x1)) {
        scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
        offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
      }
      if (((mode_sc >> 12) & 0x1) && !((mode_sc >> 1) & 0x1) && scale_bias == 0.0f &&
          offset_bias == 0.0f) {
        scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
        offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
      }
    } else if ((mode_sc >> 13) & 0x1) {
      scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }
    // Constant scaled by the guest format's minimum value (2^24-1 for D24S8, 2^24 for D24FS8) and slope
    // from subpixels to pixels (vulkan/command_processor.cpp:5786-5799).
    float bias[2] = {
        offset_bias * (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1)
                                    ? float(uint32_t(1) << 24)
                                    : float((uint32_t(1) << 24) - 1)),
        scale_bias * (1.0f / 16.0f)};
    // bo2zm_native_shadow_bias_*, only in the shadow map and only if the game does not set its own.
    if (target_shadows && type >= 4 && bias[0] == 0.0f && bias[1] == 0.0f &&
        (shadows_bias_constant_ != 0 || shadows_bias_pending_ != 0)) {
      bias[0] = float(shadows_bias_constant_) * 1000.0f;
      bias[1] = float(shadows_bias_pending_) * 0.1f;
    }
    const bool with_bias = bias[0] != 0.0f || bias[1] != 0.0f;
    if (with_bias && warnings_bias_ < 8 &&
        std::memcmp(bias, bias_warned_, sizeof(bias)) != 0) {
      ++warnings_bias_;
      std::memcpy(bias_warned_, bias, sizeof(bias));
      REXLOG_INFO("[native] C6: depth bias: scale {} offset {} (mode {:08X}) -> constant {} slope {}",
                  scale_bias, offset_bias, mode_sc, bias[0], bias[1]);
    }
    CutSubstage(16, t_indices_185);  // depth bias
    uint32_t vmin = UINT32_MAX;
    uint32_t vmax = 0;
    bool with_indices = false;
    bool indices_16 = false;  // in indices16_ instead of indices_
    indices_.clear();
    indices_known_ = nullptr;  // DedupeIndices: set by the 16-bit path below
    indices_pool_ = 0;  // bo2zm_native_index_cache, set by the 16-bit path below
    indices_scanned_ = true;
    if (source_kind == uint32_t(xenos::SourceSelect::kDMA)) {
      const uint32_t size = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
      const auto order = static_cast<xenos::Endian>(size >> 30);
      const uint32_t bytes = indices32 ? 4 : 2;
      const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
      if (count > (size & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(count) * bytes >
                                               kMemoryPhysical) {
        if (warnings_indices_ < 8) {
          ++warnings_indices_;
          REXLOG_WARN("[native] C6 diag: indices: count {} words {} VGT_DMA_SIZE {:08X} VGT_DMA_BASE {:08X} 32 bits {} "
                      "type {} VS n{} PS n{}",
                      count, size & 0xFFFFFF, size, r[gr::XE_GPU_REG_VGT_DMA_BASE], indices32,
                      type, p.vs->number, ps ? int(ps->number) : -1);
        }
        return Reject(7, "indices outside their buffer");
      }
      const uint8_t* data = memory_->TranslatePhysical(base & 0x1FFFFFFF);
      const uint32_t index_restart = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
      if (!indices32 && !restart && !displacement && !quads &&
          (order == xenos::Endian::k8in16 || order == xenos::Endian::kNone)) {
        // The common case: 16 bits without restart or offset. They are uploaded as 16 bits, with a
        // branch-free loop the compiler can vectorize.
        indices16_.resize(count);  // uninitialized (UninitializedAllocator): filled by the scan
        uint16_t* output = indices16_.data();
        uint32_t min = 0xFFFF;
        uint32_t max = 0;
        // A converted copy in the vertex cache's pools whose pages have no newer write stamp.
        const bool from_pool = icache_active_ && IndexCacheByWrites(data, count, order == xenos::Endian::k8in16,
                                                                    base & 0x1FFFFFFF, uint32_t(order), min, max,
                                                                    indices_pool_);
        // The same range already scanned and uploaded in this frame (DedupeIndices).
        indices_known_ = nullptr;
        if (dedupe_indices_on_ && !from_pool) {
          const uint32_t sync = g_syncs_ring.load(std::memory_order_relaxed);
          if (sync != dedupe_indices_sync_view_) {
            dedupe_indices_sync_view_ = sync;
            dedupe_indices_.Forget();
          }
          indices_known_ = dedupe_indices_.Find(base & 0x1FFFFFFF, count, uint32_t(order));
        }
        index_data_ = data;
        index_flip_ = order == xenos::Endian::k8in16;
        index_base_ = base & 0x1FFFFFFF;
        index_order_ = uint32_t(order);
        const bool check_known = indices_known_ && (dedupe_indices_checks_ < kDedupeIndicesChecks ||
                                                    (++dedupe_indices_sampled_ & 4095) == 0);
        if (from_pool) {
          indices_scanned_ = false;  // no scan and no upload: bound from the pool
        } else if (indices_known_ && !check_known) {
          min = indices_known_->min;  // the scan is skipped; the upload reuses the copy (or converts then)
          max = indices_known_->max;
          indices_scanned_ = false;
          ++dedupe_indices_hits_;
        } else {
          // With hand-written NEON and its guard (Indices16, bo2zm_native_indices_neon).
          Indices16(data, count, output, index_flip_, min, max);
          if (check_known) {
            CheckKnownIndices(count, min, max);
          }
        }
        vmin = min;
        vmax = max;
        indices_16 = true;
      } else {
        indices_.resize(count);
        uint32_t* output = indices_.data();
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t v;
          if (indices32) {
            uint32_t raw;
            std::memcpy(&raw, data + size_t(i) * 4, 4);
            v = xenos::GpuSwap(raw, order) & 0xFFFFFF;
          } else {
            uint16_t raw;
            std::memcpy(&raw, data + size_t(i) * 2, 2);
            v = xenos::GpuSwap(raw, order);
          }
          if (restart && v == index_restart) {
            output[i] = UINT32_MAX;
            continue;
          }
          v = (v + displacement) & 0xFFFFFF;
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
          output[i] = v;
        }
      }
      if (vmin > vmax) {
        return true;  // resets only
      }
      with_indices = true;
    } else {
      vmin = displacement;
      vmax = displacement + count - 1;
      if (quads) {
        indices_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
          indices_[i] = displacement + i;
        }
        with_indices = true;
      }
    }
    if (vmax - vmin >= kMaxVerticesPerDraw) {
      return Reject(8, "vertex range too large");
    }
    if (quads) {
      const size_t n = indices_.size() / 4;
      converted_.resize(n * 6);
      for (size_t q = 0; q < n; ++q) {
        const uint32_t* i = &indices_[q * 4];
        uint32_t* o = &converted_[q * 6];
        o[0] = i[0]; o[1] = i[1]; o[2] = i[2];
        o[3] = i[0]; o[4] = i[2]; o[5] = i[3];
      }
      indices_.swap(converted_);
    }
    CutSubstage(17, t_indices_185);  // indices
    // The indices stay as they come: vkCmdDrawIndexed subtracts vmin through vertexOffset.
    const uint32_t vertices = vmax - vmin + 1;

    // Bytes of each vertex binding in guest memory.
    struct VertexSource {
      const uint8_t* data;
      xenos::Endian order;
      uint32_t bytes;
      uint64_t address;  // physical, of the first byte used
    };
    // bo2zm_native_fewer_copies: every binding below writes its source or rejects the draw, and only
    // the bindings' sources are read, so the 384 bytes need no zeroing.
    std::array<VertexSource, 16> sources;
    if (!fewer_copies_) {
      sources.fill({});
    }
    if (input->bindings.size() > sources.size()) {
      return Reject(9, "too many vertex streams");
    }
    VkDeviceSize bytes_vertices = 0;
    for (size_t b = 0; b < input->bindings.size(); ++b) {
      const VertexBinding& binding = input->bindings[b];
      const uint32_t d0 = r[kRegFetch + binding.slot * 2];
      const uint32_t d1 = r[kRegFetch + binding.slot * 2 + 1];
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
        return Reject(10, "invalid vertex fetch constant");
      }
      const uint64_t address = uint64_t(d0 & 0x1FFFFFFC);
      const uint64_t available = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
      const uint64_t start = uint64_t(vmin) * binding.stride;
      uint64_t needed = uint64_t(vertices) * binding.stride;
      if (start + needed > available) {
        Warn(11, "vertices past the end of their buffer: clipped");
        needed = available > start ? (available - start) / binding.stride * binding.stride : 0;
      }
      if (!needed || address + start + needed > kMemoryPhysical) {
        return Reject(12, "vertices outside memory");
      }
      sources[b] = {memory_->TranslatePhysical(uint32_t(address + start)),
                     static_cast<xenos::Endian>(d1 & 0x3), uint32_t(needed),
                     address + start};
      // Its vertex cache set and stamp line, fetched before the uploads stage looks them up.
      if (prefetch_lines_ && vcache_active_ && vcache_by_writes_) {
        PrefetchCacheLines(((address + start) & 0x1FFFFFFF) | (uint64_t(uint32_t(needed) & 0x3FFFFFF) << 29) |
                               (uint64_t(d1 & 0x3) << 55) | (uint64_t(1) << 63),
                           uint32_t(address + start));
      }
      if (needed >= 4096) {
        g_probe_vertex_address.store(uint32_t(address + start), std::memory_order_relaxed);
      }
      bytes_vertices += (needed + 3) & ~VkDeviceSize(3);
    }

    // Diagnostic: one line per combination of VS, PS and render target (at most 32).
    if (diagnostics_ < 32) {
      const uint64_t key_diagnostic = (uint64_t(p.vs->number) << 44) ^
                                         (uint64_t(ps ? ps->number + 1 : 0) << 32) ^ keys[0] ^
                                         (no_clip ? 1 : 0);
      if (diagnosed_.insert(key_diagnostic).second) {
        ++diagnostics_;
        float position[4] = {};
        for (const VertexAttribute& a : input->attributes) {
          if (a.location != 0) {
            continue;
          }
          const VertexSource& source = sources[a.binding];
          if (source.bytes >= a.offset + 16) {
            for (uint32_t k = 0; k < 4; ++k) {
              uint32_t word;
              std::memcpy(&word, source.data + a.offset + k * 4, 4);
              position[k] = Float(xenos::GpuSwap(word, source.order));
            }
          }
          break;
        }
        REXLOG_INFO(
            "[native] C6 diag: VS n{} PS n{} type {} count {} {} VTE {:08X} CLIP {:08X} vp x {}+-{} y {}+-{} z {}+{} "
            "window {:08X} sc {:08X} surface {:08X} color0 {:08X} mask {:08X} depth {:08X} edram {} vtx {:08X} pos0 "
            "({:.3f} {:.3f} {:.3f} {:.3f}){}",
            p.vs->number, ps ? int(ps->number) : -1, type, count,
            source_kind == 0 ? "indices" : "auto",
            r[gr::XE_GPU_REG_PA_CL_VTE_CNTL], r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL],
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]), r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET],
            mode_sc, r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_COLOR_INFO],
            r[gr::XE_GPU_REG_RB_COLOR_MASK], control_depth, mode_edram,
            r[gr::XE_GPU_REG_PA_SU_VTX_CNTL], position[0], position[1], position[2], position[3],
            no_clip ? " [no clip]" : "");
      }
    }

    CutSubstage(18, t_indices_185);  // vertices and diagnostics
    Stage(1, mark);
    // --- Textures and samplers ----------------------------------------------------
    // In shared_buffers_[shared_buffer_] (the other one holds the previous draw's, see
    // bo2zm_native_shared_swap below); zeroed here, or with bo2zm_native_fewer_copies only when the sampler loop runs
    // (a reuse copies all of it).
    uint32_t* const shared = shared_buffers_[shared_buffer_];
    if (!fewer_copies_) {
      std::memset(shared, 0, kSharedBytes);
    }
    VkDeviceSize bytes_textures = 0;
    DiscardHashesPlanned();  // those of the previous draw that never reached UploadTexture
    textures_a_upload_.clear();
    const auto t_samplers =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    /*
     * bo2zm_native_lazy_depth. In the final composite without blur, the depth sample (HEIGHTMAP)
     * is dead: C2 is told so that requesting it does not force a copy. The same value decides
     * kSpecNoBlur further down, so textures and specialization always agree.
     */
    const bool composite_no_blur =
        ps && ps->shader && ps->shader->hash == kHashComposite && no_blur_frame_;
    const uint32_t mip_min = bo2zm::native::TextureMipFloor(
        p.vs->fetch_full ? p.vs->hash_no_fetch : p.vs->hash,
        r[gr::XE_GPU_REG_RB_DEPTHCONTROL], preset_mip_min_, diag_mip_min_);
    bool reads_reflection = false;  // reflection visibility
    /*
     * The previous draw's texture words, reused (bo2zm_native_reuse_textures). The sampler loop below
     * is a pure function of the pixel shader's samplers, the fetch constants (0x4800-0x48BF), the draw's mip floor,
     * the texture caches' generation, the frame and the descriptor slots still being current; shared[] holds all
     * it hands on (the uploads it queues were made by the earlier draw, whose uploads stage succeeded). When none of
     * those changed since the previous draw, the loop would produce the same words, so they are copied instead:
     * on the console the texture stage is 3.7-4.3 us of the ~10.5 us a draw costs. Guarded: the first
     * kReuseTexturesChecks reuses, then 1 in 1,024, still run the loop and compare; a difference turns reuse off
     * for the rest of the run with a MISMATCH line.
     *
     * The fetch constants: the loop reads only the six words of each of the pixel shader's sampler slots (0-15).
     * The generation covers the whole block 0x4800-0x48BF, vertex fetch constants included, which change between
     * most draws of the same material. So when the generation moved, those six words per sampler are compared with
     * the snapshot's: equal words give the loop the same input (reuse_fetch_, "by value" in the report).
     */
    const uint64_t generation_textures_before = generation_textures_;
    const uint64_t slots_retired_before = slots_retired_;
    // Vertex-shader samplers are not part of the reuse key: their draws always run the loop.
    bool reuse_textures = reuse_textures_on_ && reuse_valid_ && ps == reuse_ps_ && p.vs->samplers.empty() &&
                          mip_min == reuse_mip_min_ && !composite_no_blur &&
                          generation_textures_ == reuse_generation_textures_ && frame_ == reuse_frame_ &&
                          slots_retired_ == reuse_slots_retired_;
    if (reuse_textures && p.generation_fetch != reuse_generation_fetch_) {
      reuse_textures = SameSamplerFetch(ps, r);
      if (reuse_textures) {
        reuse_generation_fetch_ = p.generation_fetch;  // the next draws with no fetch write take the fast check
        ++reuse_by_value_;
      }
    }
    const bool check_reuse =
        reuse_textures && (reuse_checks_ < kReuseTexturesChecks || (++reuse_sampled_ & 1023) == 0);
    if (reuse_textures && !check_reuse) {
      std::memcpy(shared, reuse_shared_, kSharedBytes);
      reads_reflection = reuse_reads_reflection_;
      ++reuse_hits_;
    } else {
    if (fewer_copies_) {
      std::memset(shared, 0, kSharedBytes);
    }
    {  // every slot's fetch scale starts at 1
      const float one = 1.0f;
      for (uint32_t k = 0; k < kSlotsPs + kSlotsVs; ++k) {
        std::memcpy(shared + WordScale(k), &one, sizeof(one));
        std::memcpy(shared + WordScaleCoords(k), &one, sizeof(one));
        std::memcpy(shared + WordScaleCoords(k) + 1, &one, sizeof(one));
      }
    }
    // The samplers to prepare: the pixel shader's (slot = fetch constant 0-15) and the vertex shader's (CTAB
    // register k reads fetch constant 16 + k, slot 16 + k).
    std::array<uint32_t, kSlotsPs + kSlotsVs> slots_draw;
    size_t slots_count = 0;
    if (ps) {
      for (const SamplerShader& x : ps->samplers) {
        if (x.reg_index < kSlotsPs && slots_count < slots_draw.size()) slots_draw[slots_count++] = x.reg_index;
      }
    }
    for (const SamplerShader& x : p.vs->samplers) {
      if (x.reg_index < kSlotsVs && slots_count < slots_draw.size()) {
        slots_draw[slots_count++] = kSlotsPs + x.reg_index;
      }
    }
    if (composite_no_blur) {
      context_->DeadDepthReads(true);
    }
    for (size_t i_slot = 0; i_slot < slots_count; ++i_slot) {
      const uint32_t slot = slots_draw[i_slot];
      const uint32_t* fetch = r + kRegFetch + slot * 6;
      // reflection visibility. Here and not in TextureResolved, which, because of the caches, only sees
      // the first draw of each frame that samples it. Without checking the dimension: over-counting only
      // means the reflection gets drawn.
      if (measure_visibility_ && (fetch[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((fetch[1] & 0xFFFFF000u) & 0x1FFFFFFFu) == bo2zm::reflection_demand::kAddress) {
        reads_reflection = true;
      }
      ++samplers_prepared_;
      // Each sampler's time by the path it took, on the timed draws.
      const auto t_one = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      uint32_t path = 20;  // the table hit, unless a branch below says otherwise
      bool no_cache = false;  // depth requested but not sampled, kept out of the caches
      // Per-register cache: same fetch constant and mip floor, with no intervening C2 copy.
      CacheSampler& cache = cache_samplers_[slot];
      // Valid until cache.valid_until (with the cross-frame cache disabled, only its own frame)
      if ((cache_between_frames_ ? frame_ <= cache.valid_until : cache.frame == frame_) &&
          cache.generation == generation_textures_ && cache.mip_min == mip_min && SlotCurrent(cache) &&
          std::memcmp(cache.fetch.data(), fetch, sizeof(cache.fetch)) == 0) {
        shared[WordTexture(cache.heap, slot)] = cache.slot;
        shared[WordSampler(slot)] = cache.sampler;
        WriteInvSize(shared, slot, cache.width, cache.height);
        WriteScale(shared, slot, fetch);
        WriteScaleCoords(shared, slot, fetch, cache.width, cache.height);
        ++samplers_cache_;
        SamplerPath(19, t_one);
        continue;
      }
      uint32_t slot_texture = 0;
      uint32_t heap = 0;
      uint32_t slot_sampler = 0;
      uint32_t width_host = 0, height_host = 0;
      // Second cache, keyed by the whole fetch constant and mip floor in the same generation: a register
      // changes texture between draws, but textures repeat a lot within a frame, and PrepareTexture already
      // queued their upload the first time.
      // Two ways per set, the two adjacent entries the hash picks. Direct-mapped, two fetch constants
      // sharing a slot evict each other on every use: 4,500-5,600 misses per 10 s on the console (7,700-8,000 on
      // the PC), each a trip through PrepareTexture. A fetch constant is looked for in both ways;
      // if neither holds it, it replaces an empty (or invalidated) way, otherwise the one whose validity ends first.
      const size_t set = (XXH3_64bits(fetch, sizeof(uint32_t) * 6) ^ (uint64_t(mip_min) << 1)) &
                         (cache_fetch_.size() - 2);
      CacheSampler* via = &cache_fetch_[set];
      if (via->mip_min != mip_min || std::memcmp(via->fetch.data(), fetch, sizeof(via->fetch)) != 0) {
        CacheSampler* const other = &cache_fetch_[set + 1];
        if ((other->mip_min == mip_min && std::memcmp(other->fetch.data(), fetch, sizeof(other->fetch)) == 0) ||
            other->frame == UINT64_MAX ||
            (via->frame != UINT64_MAX && other->valid_until < via->valid_until)) {
          via = other;
        }
      }
      CacheSampler& per_fetch = *via;
      uint64_t valid_until = frame_;
      if ((cache_between_frames_ ? frame_ <= per_fetch.valid_until : per_fetch.frame == frame_) &&
          per_fetch.generation == generation_textures_ && per_fetch.mip_min == mip_min && SlotCurrent(per_fetch) &&
          std::memcmp(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch)) == 0) {
        slot_texture = per_fetch.slot;
        heap = per_fetch.heap;
        slot_sampler = per_fetch.sampler;
        valid_until = per_fetch.valid_until;
        width_host = per_fetch.width;
        height_host = per_fetch.height;
        ++samplers_cache_fetch_;
      } else {
        // Why the table misses ("C6 sampler cache by fetch" report, every 10 s).
        if (per_fetch.mip_min != mip_min || std::memcmp(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch)) != 0) {
          ++(per_fetch.frame == UINT64_MAX ? fetch_misses_empty_ : fetch_misses_collision_);
        } else if (per_fetch.generation != generation_textures_ || !SlotCurrent(per_fetch)) {
          ++fetch_misses_generation_;
        } else {
          ++fetch_misses_expired_;
        }
        bool sampling_point = false;
        path = 21;
        PrepareTexture(fetch, slot_texture, heap, bytes_textures, sampling_point, valid_until,
                        width_host, height_host);
        slot_sampler = SlotSampler(fetch, sampling_point, mip_min);
        per_fetch.frame = frame_;
        per_fetch.generation = generation_textures_;
        per_fetch.mip_min = mip_min;
        std::memcpy(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch));
        cache_fetch_dir_[size_t(&per_fetch - cache_fetch_.data())] = AddressCacheSampler(fetch);
        per_fetch.slot = slot_texture;
        per_fetch.heap = heap;
        per_fetch.generation_slot = GenerationSlot(heap, slot_texture);
        per_fetch.sampler = slot_sampler;
        per_fetch.valid_until = valid_until;
        per_fetch.width = width_host;
        per_fetch.height = height_host;
        // A depth requested without being sampled is not stored in the caches: the next draw that really
        // samples it has to go through TextureResolved, which is where it is recorded if it was deferred.
        if (composite_no_blur && sampling_point) {
          per_fetch.frame = 0;
          per_fetch.valid_until = 0;
          valid_until = 0;
          no_cache = true;
        }
      }
      shared[WordTexture(heap, slot)] = slot_texture;
      shared[WordSampler(slot)] = slot_sampler;
      WriteInvSize(shared, slot, width_host, height_host);
      WriteScale(shared, slot, fetch);
      WriteScaleCoords(shared, slot, fetch, width_host, height_host);
      cache.frame = frame_;
      cache.generation = generation_textures_;
      cache.mip_min = mip_min;
      std::memcpy(cache.fetch.data(), fetch, sizeof(cache.fetch));
      cache.slot = slot_texture;
      cache.heap = heap;
      cache.generation_slot = GenerationSlot(heap, slot_texture);
      cache.sampler = slot_sampler;
      cache.valid_until = valid_until;
      cache.width = width_host;
      cache.height = height_host;
      if (no_cache) {
        cache.frame = 0;  // with valid_until at 0 it is not valid across frames either
      }
      SamplerPath(path, t_one);
      if (path == 21) {
        SamplerPath(22 + (prepare_new_ ? 5 : prepare_outcome_), t_one);  // and by what PrepareTexture did
      }
    }
    if (composite_no_blur) {
      context_->DeadDepthReads(false);
    }
    if (check_reuse) {
      CheckReuseTextures(shared, reads_reflection, ps);
    }
    }  // the sampler loop
    // What the next draw may reuse; valid once this draw's uploads succeed (reuse_valid_, uploads stage).
    // bo2zm_native_fewer_copies: after a reuse the words are reuse_shared_'s already, and the snapshot
    // still holds the sampler slots of the same PS they were made with (equal by generation or by value).
    if (!(fewer_copies_ && reuse_textures && !check_reuse)) {
      std::memcpy(reuse_shared_, shared, kSharedBytes);
      std::memcpy(reuse_fetch_, r + kRegFetch, sizeof(reuse_fetch_));  // texture slots 0-15 (SameSamplerFetch)
    }
    reuse_reads_reflection_ = reads_reflection;
    reuse_ps_ = ps;
    reuse_mip_min_ = mip_min;
    reuse_generation_fetch_ = p.generation_fetch;
    reuse_generation_textures_ = generation_textures_;
    reuse_frame_ = frame_;
    reuse_slots_retired_ = slots_retired_;
    reuse_valid_ = false;
    reuse_pending_ = !composite_no_blur && generation_textures_ == generation_textures_before &&
                     slots_retired_ == slots_retired_before;
    /*
     * bo2zm_native_shadow_min. If this draw samples the shadow map texture with the overlay (the game's
     * texture[0]) and C2 holds it with only the overlay draws, its pair (the base map) goes in the 3D
     * index word of that register (the shadow map is 2D: that word is unused) and the pipeline carries
     * kSpecShadowMin: tfetch2DShadowMin takes the minimum of the two, the same value the copy would have
     * left. While C2 is observing, the pair is the texture itself (the same texel), so the pipelines with
     * the bit already exist when applying starts. Every read is reported to C2, which decides and watches.
     */
    bool shadow_min_draw = false;
    if (ps && !ps->samplers.empty()) {
      if (const uint32_t dir_overlay = context_->AddressShadowOverlay()) {
        for (const SamplerShader& s : ps->samplers) {
          if (s.reg_index >= 16) {
            continue;
          }
          const uint32_t* f = r + kRegFetch + uint32_t(s.reg_index) * 6;
          if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
              (((f[1] >> 12) << 12) & 0x1FFFFFFF) != dir_overlay) {
            continue;
          }
          const bool capable = ps->shadow_min && s.map_shadows &&
                             ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked);
          const NativeImage* pair = context_->PartnerShadowOverlay(capable, ps->number);
          if (!pair) {
            continue;
          }
          const uint32_t slot_pair =
              SlotView(pair->image, pair->format, (f[3] >> 1) & 0xFFF, kSwizzleRRRR);
          if (!slot_pair) {
            context_->PartnerShadowOverlay(false, ps->number);  // no view, no minimum: C2 records it
            continue;
          }
          shared[16 + s.reg_index] = slot_pair;
          shadow_min_draw = true;
        }
      }
    }

    if (time_) {  // C6 substages: the sampler loop inside the textures stage
      sub_ns_[14] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_samplers)
                                  .count());
      ++sub_n_[14];
    }
    Stage(2, mark);
    // --- Upload buffer space ----------------------------------------------------------
    const VkDeviceSize bytes_indices =
        indices_16 ? VkDeviceSize(indices16_.size()) * 2 : VkDeviceSize(indices_.size()) * 4;
    // With a single binding the copy goes to a multiple of the stride: up to stride - 4 bytes of padding.
    const VkDeviceSize gap_base_zero =
        vertices_base_cero_ && input->bindings.size() == 1 ? VkDeviceSize(input->bindings[0].stride) : 0;
    const VkDeviceSize needed = gap_base_zero + bytes_vertices + bytes_indices + bytes_textures +
                                    2 * VkDeviceSize(kRegistersConstants) * 4 +
                                    kWordsShared * 4 + 64 * 8 +
                                    // With UBOs the blocks go whole and aligned to alignment_ubo_
                                    (use_ubo_ ? kUboBytesVs + kUboBytesPs + kUboBytesShared +
                                                     3 * alignment_ubo_
                                               : 0);
    if (needed > kSizeUpload) {
      return Reject(13, "draw larger than the upload buffer");
    }
    const bool shared_full =
        shared_separate_ && use_ubo_ &&
        shared_used_ + kUboBytesShared + alignment_ubo_ > kSizeShared;
    if (upload_used_ + needed > kSizeUpload || shared_full) {
      const auto before_submit = std::chrono::steady_clock::now();
      const bool submitted = context_->SubmitYWait();
      ++submits_full_;
      ns_submits_full_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - before_submit)
                                        .count());
      if (!submitted) {
        return false;
      }
    }

    // --- Render pass ----------------------------------------------------------------
    /*
     * No XXH3 on every draw (bo2zm_native_fast_pass_key). If the 40 bytes of render targets are the
     * same ones that produced pass_key_ (BeginPass stores them next to it), their XXH3 is pass_key_.
     * Otherwise it is computed as usual: the pass change decision is the same as without the shortcut in
     * every case, collisions included.
     */
    uint64_t key_pass;
    if (key_pass_fast_ && pass_keys_valid_ && std::memcmp(keys, pass_keys_, sizeof(keys)) == 0) {
      const uint64_t n = ++keys_pass_fast_;
      key_pass = (n <= kKeysPassACheck || (n & 4095) == 0) ? CheckKeyPass(keys, n) : pass_key_;
    } else {
      key_pass = XXH3_64bits(keys, sizeof(keys));
    }
    if (!pass_active_ || key_pass != pass_key_ ||
        pass_generation_ != context_->GenerationCommands()) {
      const auto before_pass = std::chrono::steady_clock::now();
      if (pass_generation_ != context_->GenerationCommands()) {
        ++passes_per_generation_;
      } else if (!pass_active_ && key_pass == pass_key_) {
        ++passes_resumed_;
      } else {
        ++passes_per_target_;
      }
      FinishPass();
      // The pass change is split. FinishPass closes the previous pass; BeginPass finds or creates the
      // render targets (it may create images, record barriers and clear them), builds the render pass and
      // the framebuffer, and opens the pass.
      const auto after_finish = std::chrono::steady_clock::now();
      if (time_) {
        stages_ns_[8] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            after_finish - before_pass).count());
      }
      const bool begun = BeginPass(r, keys, pitch, key_pass, ps);
      const auto after_pass = std::chrono::steady_clock::now();
      ns_passes_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      // The pass change, separate from what the stage costs on each draw.
      if (time_) {
        stages_ns_[7] += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      }
      if (!begun) {
        return false;
      }
    }
    const VkCommandBuffer cmd = pass_commands_;
    if (recording_generation_ != context_->GenerationCommands()) {
      recording_generation_ = context_->GenerationCommands();
      pipeline_bound_ = VK_NULL_HANDLE;
      eds_valid_ = false;  // phases 1 and 2: nothing is set in the new buffer
      key_bound_valid_ = false;  // New command buffer (CountChangePipeline)
      sets_bound_ = false;
      ubo_bound_ = false;
      state_recorded_ = false;
      bindings_recorded_ = 0;  // different command buffer: nothing is bound
    }

    Stage(3, mark);
    // --- Uploads: textures, vertices, indices and constants ------------------------------
    // The stage's parts (C6 substages 36-39), like the texture stage's samplers.
    auto t_uploads = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (CachedTexture* texture : textures_a_upload_) {
      if (!UploadTexture(*texture)) {
        return false;
      }
    }
    CutSubstage(36, t_uploads);
    reuse_valid_ = reuse_pending_ && generation_textures_ == reuse_generation_textures_ &&
                   slots_retired_ == reuse_slots_retired_;  // see reuse_textures (textures stage)
    std::array<VkDeviceSize, 16> offsets_vertices{};
    // If the guest has waited for the GPU since the last draw, it may legally have rewritten an already
    // referenced range: what was recorded is no longer valid.
    if (dedupe_active_) {
      const uint32_t sync = g_syncs_ring.load(std::memory_order_relaxed);
      if (sync != dedupe_sync_view_) {
        dedupe_sync_view_ = sync;
        dedupe_.Forget();
      }
    }
    const auto before_vertices =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (size_t b = 0; b < input->bindings.size(); ++b) {
      const VertexSource& source = sources[b];
      // Each binding's time by path (C6 substages 40-42).
      const auto t_binding = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      if (diag_vertices_repeated_) {
        AnnotateVerticesRepeated(source.address, source.bytes, uint32_t(source.order), source.data);
      }
      VkDeviceSize offset;
      // In the dedupe's check frames every range carries the hash of its guest bytes; a reuse whose
      // bytes differ is counted (DedupeVertices::discrepancies) and copied as usual.
      const uint64_t hash_dedupe =
          dedupe_active_ && dedupe_hash_frame_ ? XXH3_64bits(source.data, size_t(source.bytes)) | 1 : 0;
      // If this same range was already copied in this frame, its place in the upload buffer is reused and
      // nothing is copied. See bo2zm_native_vertex_dedupe.h.
      if (dedupe_active_ &&
          dedupe_.Find(source.address, source.bytes, uint32_t(source.order), offset, hash_dedupe)) {
        offsets_vertices[b] = offset;
        SamplerPath(40, t_binding);
        continue;
      }
      // GPU skinning (bo2zm_gpu_skin.h). A range that holds skinning jobs' output is skinned by compute
      // dispatches into its place in the upload buffer instead of copied (the parts without a job are copied).
      if (skin_on_ && bo2zm::gpu_skin::MayOverlap(uint32_t(source.address), source.bytes) &&
          SkinOnGpu(source.data, source.order, source.bytes, source.address, input->bindings[b].stride,
                    input->bindings.size() == 1, offset)) {
        offsets_vertices[b] = offset;
        bytes_vertices_ += source.bytes;
        if (dedupe_active_) {
          dedupe_.Annotate(source.address, source.bytes, uint32_t(source.order), offset, hash_dedupe);
        }
        SamplerPath(42, t_binding);
        continue;
      }
      // bo2zm_native_vertex_cache: bound from a persistent copy when its bytes did not change (the offset then
      // carries kVertexPoolTag; see ResolveVertexBinding).
      if (vcache_active_) {
        const VkDeviceSize alignment =
            vertices_base_cero_ && input->bindings.size() == 1 ? VkDeviceSize(input->bindings[0].stride) : 4;
        uint64_t hash_cache = hash_dedupe;
        if (VertexCache(source.data, source.bytes, source.order, source.address, alignment, hash_cache, offset)) {
          offsets_vertices[b] = offset;
          if (dedupe_active_) {
            dedupe_.Annotate(source.address, source.bytes, uint32_t(source.order), offset, hash_dedupe);
          }
          SamplerPath(41, t_binding);
          continue;
        }
      }
      // With a single binding, at a multiple of its stride (bo2zm_native_zero_base_vertices).
      if (vertices_base_cero_ && input->bindings.size() == 1) {
        ReserveMultiple(source.bytes, input->bindings[0].stride, offset);
      } else {
        Reserve(source.bytes, 4, offset);
      }
      const CopyJob job{source.data, upload_data_ + offset, source.bytes / 4, source.order};
      if (!copies_active_ || !EnqueueCopy(job)) {
        CopyVertices(job);
      }
      if (watch_vertices_) [[unlikely]] {
        WatchVerticesCollect(source.address, source.bytes);
      }
      bytes_vertices_ += source.bytes;
      offsets_vertices[b] = offset;
      if (dedupe_active_) {
        dedupe_.Annotate(source.address, source.bytes, uint32_t(source.order), offset, hash_dedupe);
      }
      SamplerPath(42, t_binding);
    }
    if (time_) {
      ns_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - before_vertices)
                                   .count());
    }
    CutSubstage(37, t_uploads);
    VkDeviceSize offset_indices = 0;
    if (with_indices) {
      if (indices_pool_) {
        offset_indices = indices_pool_;  // in the index cache's pool (tagged as the vertex cache's)
        bytes_indices_saved_ += bytes_indices;
      } else if (indices_16 && indices_known_ && indices_known_->frame == dedupe_indices_.frame()) {
        // Already in this upload buffer (DedupeIndices).
        offset_indices = indices_known_->offset;
        bytes_indices_saved_ += bytes_indices;
      } else {
        Reserve(bytes_indices, 4, offset_indices);
        bytes_indices_uploaded_ += bytes_indices;
        if (indices_16 && indices_known_ && !indices_scanned_) {
          // The scan was skipped and the upload buffer changed since (a submit in between): convert from the
          // guest's indices, which a draw cannot change, straight into the upload buffer.
          uint32_t mn = 0;
          uint32_t mx = 0;
          Indices16Neon(index_data_, uint32_t(bytes_indices / 2),
                        reinterpret_cast<uint16_t*>(upload_data_ + offset_indices), index_flip_, mn, mx);
          ++dedupe_indices_converted_;
        } else {
          std::memcpy(upload_data_ + offset_indices,
                      indices_16 ? static_cast<const void*>(indices16_.data())
                                    : static_cast<const void*>(indices_.data()),
                      size_t(bytes_indices));
        }
        if (indices_16 && dedupe_indices_on_) {
          // While reuses are checked, the guest bytes' hash goes with the entry (CheckKnownIndices).
          const uint64_t hash = dedupe_indices_checks_ < kDedupeIndicesChecks || (dedupe_indices_sampled_ & 4095) < 64
                                    ? XXH3_64bits(index_data_, size_t(bytes_indices))
                                    : 0;
          dedupe_indices_.Annotate(index_base_, uint32_t(bytes_indices / 2), index_order_, uint16_t(vmin),
                                   uint16_t(vmax), offset_indices, hash);
        }
      }
    }
    indices_known_ = nullptr;
    indices_scanned_ = true;
    CutSubstage(38, t_uploads);
    // Only the registers each shader reads (ShaderInput::constants_bytes). With the same generation, the
    // copy is reused if it already covers what this shader needs.
    const uint32_t bytes_vs = std::min<uint32_t>(p.vs->constants_bytes, kRegistersConstants * 4);
    // bo2zm_native_constants_used: a block copied in part holds only its shader's registers.
    const bool vs_runs = constants_used_active_ && p.vs->constants_runs_valid;
    if (constants_vs_generation_ != p.generation_constants_vs ||
        constants_vs_epoch_ != epoch_upload_ || bytes_vs > constants_vs_bytes_ ||
        (constants_vs_shader_ && constants_vs_shader_ != p.vs)) {
      // Why they are uploaded again (measurement only; each upload is another set 4 offset).
      ++reuploads_vs_[constants_vs_generation_ != p.generation_constants_vs ? 0
                      : constants_vs_epoch_ != epoch_upload_                 ? 1
                                                                              : 2];
      // With UBOs the block goes whole (the driver may read the full range) and aligned to the device minimum
      const auto t_copy = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_vs, kUboBytesVs) : bytes_vs, use_ubo_ ? alignment_ubo_ : 16,
               constants_vs_offset_);
      if (vs_runs) {
        for (const auto& [first, n] : p.vs->constants_runs) {
          std::memcpy(upload_data_ + constants_vs_offset_ + first * 16u, r + kRegConstantsVs + first * 4u, n * 16u);
        }
        bytes_constants_vs_ += p.vs->constants_runs_bytes;
      } else {
        std::memcpy(upload_data_ + constants_vs_offset_, r + kRegConstantsVs, bytes_vs);
        bytes_constants_vs_ += bytes_vs;
      }
      constants_vs_shader_ = vs_runs ? p.vs : nullptr;
      SamplerPath(43, t_copy);  // C6 substage
      constants_vs_generation_ = p.generation_constants_vs;
      constants_vs_epoch_ = epoch_upload_;
      constants_vs_bytes_ = bytes_vs;
    }
    const uint32_t bytes_ps =
        ps ? std::min<uint32_t>(ps->constants_bytes, kRegistersConstants * 4) : 0;
    const bool ps_runs = ps && constants_used_active_ && ps->constants_runs_valid;
    if (ps && (constants_ps_generation_ != p.generation_constants_ps ||
               constants_ps_epoch_ != epoch_upload_ || bytes_ps > constants_ps_bytes_ ||
               (constants_ps_shader_ && constants_ps_shader_ != ps))) {
      ++reuploads_ps_[constants_ps_generation_ != p.generation_constants_ps ? 0
                      : constants_ps_epoch_ != epoch_upload_                 ? 1
                                                                              : 2];
      const auto t_copy = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps, use_ubo_ ? alignment_ubo_ : 16,
               constants_ps_offset_);
      if (ps_runs) {
        for (const auto& [first, n] : ps->constants_runs) {
          // A PS reads at most 224 registers of its block (bytes_ps); the analysis never names more.
          if (first * 16u + n * 16u <= bytes_ps) {
            std::memcpy(upload_data_ + constants_ps_offset_ + first * 16u, r + kRegConstantsPs + first * 4u,
                        n * 16u);
          }
        }
        bytes_constants_ps_ += ps->constants_runs_bytes;
      } else {
        std::memcpy(upload_data_ + constants_ps_offset_, r + kRegConstantsPs, bytes_ps);
        bytes_constants_ps_ += bytes_ps;
      }
      constants_ps_shader_ = ps_runs ? ps : nullptr;
      SamplerPath(44, t_copy);  // C6 substage
      constants_ps_generation_ = p.generation_constants_ps;
      constants_ps_epoch_ = epoch_upload_;
      constants_ps_bytes_ = bytes_ps;
    }
    CopyCompositeTreated(ps, r, bytes_ps);  // bo2zm_visual_treatment (see the function)
    CutSubstage(39, t_uploads);

    Stage(4, mark);
    // The stage's parts (C6 substages 48-50).
    auto t_pipeline = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // --- Viewport, scissor and shared constants ---------------------------------------
    /*
     * The framing (viewport, ndc and scissor), cached (bo2zm_native_framing_cache).
     *
     * The viewport, the ndc and the scissor are a function of the framing registers (PA_CL_VTE_CNTL,
     * PA_CL_VPORT_*, PA_SC_WINDOW_OFFSET/SCISSOR, PA_CL_CLIP_CNTL and PA_SU_SC_MODE_CNTL:
     * IsFramingRegister in bo2zm_native_render.cpp, which bumps generation_framing on every path that
     * writes them) and of the pass size and scale (pass_series_). In gameplay the framing really changes
     * 0.07-0.14 times per draw ("C6 generations"). The depth bias is not included: it depends on
     * PA_SU_POLY_OFFSET_*, RB_DEPTH_INFO and the primitive type, which do not bump that generation.
     */
    const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
    gpu::Viewport viewport{};
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    VkRect2D scissor{};
    uint32_t framing_empty = 0;
    const bool framing_in_cache = framing_cache_ && framing_valid_ &&
                                   p.generation_framing == framing_generation_ && pass_series_ == framing_pass_series_;
    bool check_framing = false;
    if (framing_in_cache) {
      const uint64_t n = ++framing_hits_;
      check_framing = n <= kFramingsACheck || (n & 4095) == 0;
      ++framing_hits_report_;
    }
    if (framing_in_cache && !check_framing) {
      viewport = framing_viewport_;
      std::memcpy(ndc, framing_ndc_, sizeof(ndc));
      scissor = framing_scissor_;
      framing_empty = framing_empty_;
    } else {
      framing_empty = ComputeFraming(r, vte, no_clip, mode_sc, viewport, ndc, scissor);
      ++framing_calcs_report_;
      if (check_framing) {
        CompareFraming(viewport, ndc, scissor, framing_empty);  // with the stored values, before overwriting them
      }
      framing_viewport_ = viewport;
      std::memcpy(framing_ndc_, ndc, sizeof(ndc));
      framing_scissor_ = scissor;
      framing_empty_ = framing_empty;
      framing_generation_ = p.generation_framing;
      framing_pass_series_ = pass_series_;
      framing_valid_ = true;
    }
    if (framing_empty != 0) {
      return true;  // empty viewport or scissor: nothing to draw
    }

    uint32_t specialization = input->specialization;
    if (composite_no_blur) {  // the same value that decided the textures
      specialization |= kSpecNoBlur;
    }
#ifndef BO2ZM_NO_VARIANTS_GLOW
    if (ps && ps->shader && ps->shader->hash == kHashBrightPass) {
      const int mode = GlowSky();
      specialization |= mode == 1 ? kSpecGlowNatural : mode == 2 ? kSpecGlowSmooth : 0;
    }
#endif
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    float threshold_alpha = 0.0f;
    uint32_t function_alpha = 7;  // always
    // The 8 Xenos functions (0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always) with alphaTestValue
    // (the library build that validated normals). Without a PS (mode 5) there is no test.
    if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
      threshold_alpha = Float(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
      function_alpha = control_color & 0x7;
      specialization |= 0x2;
      // The function goes in the pipeline, not in the constants. RB_COLORCONTROL was already part of the key,
      // so this creates no pipelines that did not already exist.
      specialization |= (function_alpha & 0x7u) << kSpecFunctionAlphaOffset;
    }
    shared[64] = (r[kRegBooleans] & 0xFFFF) | ((r[kRegBooleans + 4] & 0xFFFF) << 16);
    for (uint32_t w = 0; w < 8; ++w) {  // loop counts, see kWordLoops (SQ_LOOP_CONST: count in bits 0-7)
      const uint32_t* loop = r + kRegBooleans + 8 + w * 4;
      shared[kWordLoops + w] = (loop[0] & 0xFF) | (loop[1] & 0xFF) << 8 | (loop[2] & 0xFF) << 16 | (loop[3] & 0xFF) << 24;
    }
    shared[65] = 0;  // g_SwappedTexcoords
    if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {  // PixelCenter::kD3DZero
      const float mid[2] = {1.0f / viewport.width, -1.0f / std::abs(viewport.height)};
      std::memcpy(&shared[66], mid, sizeof(mid));
    }
    std::memcpy(&shared[68], &threshold_alpha, sizeof(threshold_alpha));
    shared[69] = function_alpha;  // g_AlphaFunction
    std::memcpy(&shared[70], ndc, sizeof(ndc));
    std::copy(input->remaps.begin(), input->remaps.end(), shared + 74);
    VkDeviceSize offset_shared;
    /*
     * How many draws really change the shared constants.
     *
     * This block is 488 bytes that are zeroed, filled, compared and, if they changed, copied twice, once
     * into memory without CPU caching (2654 MB/s). With ~2,400 draws per frame that is ~4.7 MB of traffic
     * per frame on the thread that is already at 96 % of a core.
     *
     * Which fix applies depends on the count: if almost no draw changes the block, the comparison is the
     * waste; if almost all do, the double memcpy is. They are two different fixes, and counting costs one
     * increment.
     */
    ++shared_observed_;
    const auto t_shared =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    uint32_t* const shared_previous = shared_buffers_[shared_buffer_ ^ 1];
    if (shared_epoch_ == epoch_upload_ && std::memcmp(shared, shared_previous, kSharedBytes) == 0) {
      offset_shared = shared_offset_;  // the same as the previous draw
    } else {
      ++shared_changed_;
      if (shared_separate_ && use_ubo_) {
        // To the separate CPU-cached buffer; binding 2 of set 4 points to it.
        offset_shared = (shared_used_ + alignment_ubo_ - 1) & ~(alignment_ubo_ - 1);
        shared_used_ = offset_shared + kUboBytesShared;
        std::memcpy(shared_data_ + offset_shared, shared, kSharedBytes);
      } else {
        Reserve(use_ubo_ ? kUboBytesShared : kWordsShared * 4, use_ubo_ ? alignment_ubo_ : 16,
                 offset_shared);
        std::memcpy(upload_data_ + offset_shared, shared, kSharedBytes);
      }
      // bo2zm_native_shared_swap: this buffer becomes the previous one and the next draw builds in the
      // other, instead of copying the block.
      if (shared_swap_) {
        shared_buffer_ ^= 1;
      } else {
        std::memcpy(shared_previous, shared, kSharedBytes);
      }
      shared_offset_ = offset_shared;
      shared_epoch_ = epoch_upload_;
    }
    if (time_) {
      sub_ns_[12] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_shared)
                                  .count());
      ++sub_n_[12];
    }

    CutSubstage(48, t_pipeline);
    // --- Pipeline ---------------------------------------------------------------------------
    PipelineKey key;
    key.vs = p.vs->number + 1;
    key.ps = ps ? ps->number + 1 : 0;  // 0: mode 5, no fragment stage
    key.input = input->hash;
    key.topology = uint32_t(topology);
    if (use_ubo_) {
      specialization |= kSpecConstantsUbo;  // the shaders read from set 4
    }
    if (inv_size_tex_) {
      specialization |= kSpecInvSizeTex;
    }
    if (pcf_cheap_) {
      specialization |= kSpecPcfCheap;
    }
    if (only_alpha && ps) {
      // If vegetation is excluded from the shadow map, this whole draw is unnecessary.
      if (no_vegetation_) {
        return true;
      }
      specialization |= kSpecOnlyAlpha;
    }
    /*
     * Test depth before shading where it costs nothing.
     *
     * A pixel shader that can discard (alpha test or its own kill) forces the hardware to shade first and
     * test Z afterwards: otherwise a discarded fragment would already have written its depth. But that only
     * matters if the draw writes depth. If it only tests it (smoke, particles, glass, decals, lights),
     * testing earlier is exact: there is no write to move, the shader still discards the color, and the GPU
     * stops shading what is hidden.
     *
     * The counters are kept even with the setting off, so the report says the same in both halves of an A/B
     * test, and the "CANNOT because they write depth" line measures exactly what a depth pre-pass
     * would have to fix.
     */
    if (ps && keys[4] && masks) {
      const bool test_z = ((control_depth >> 1) & 0x1) != 0;
      const bool writes_z = test_z && ((control_depth >> 2) & 0x1) != 0;
      const bool has_stencil = (control_depth & 0x1) != 0;
      const bool ps_writes_z = (ps->outputs & 0x10) != 0;
      const bool late = (specialization & 0x2) || ps->discards || ps_writes_z;
      if (!test_z) {
        // Without a depth test there is nothing to move earlier.
      } else if (!late) {
        ++counts_z_[kZAlreadyEarly];
      } else if (writes_z || ps_writes_z) {
        ++counts_z_[kZWritesZ];
      } else if (has_stencil) {
        ++counts_z_[kZStencil];
      } else if (occlusion_open_) {
        ++counts_z_[kZOcclusion];  // the game reads those samples: leave it alone
      } else if (ps->shader && ps->shader->hash != kHashBrightPass &&
                 ps->shader->hash != kHashComposite) {
        ++counts_z_[kZSet];
        if (z_early_) {
          specialization |= kSpecZEarly;
        }
      }
    }
    // bo2zm_native_shadow_min. The shadow map pair bit (see above, textures).
    if (shadow_min_draw) {
      specialization |= kSpecShadowMin;
    }
    /*
     * bo2zm_native_shadow_min. In the overlay pass of the shadow map, C2 needs to know whether this draw
     * leaves in the depth buffer exactly the minimum of what was there and of its fragments: no stencil, no
     * occlusion query and, if it writes Z, a NEVER, LESS or LEQUAL test. The game's register
     * (RB_DEPTHCONTROL) is checked, not the pipeline key. BeginPass checks that the pass is depth-only.
     */
    if (pass_overlay_shadow_) {
      const uint32_t control_z_overlay = keys[4] ? control_depth : 0;
      const bool writes_z_overlay = ((control_z_overlay >> 1) & 0x1) && ((control_z_overlay >> 2) & 0x1);
      const uint32_t function_z_overlay = (control_z_overlay >> 4) & 0x7;
      context_->OverlayShadowDraw(!(control_z_overlay & 0x1) && !occlusion_open_ &&
                                          (!writes_z_overlay || function_z_overlay == 0 || function_z_overlay == 1 ||
                                           function_z_overlay == 3),
                                      control_z_overlay);
    }
    key.specialization = specialization;
    std::copy(std::begin(pass_formats_), std::end(pass_formats_), std::begin(key.formats));
    static constexpr uint32_t kBlend[4] = {
        gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
        gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        key.blend[i] = r[kBlend[i]] & 0x1FFF1FFF;
      }
    }
    key.masks = masks;
    key.depth = keys[4] ? control_depth : 0;
    // Diagnostics (bo2zm_native_debug_no_depth): every draw without depth or stencil testing.
    if (bo2zm::native::DebugNoDepth()) {
      key.depth = 0;
    }
    if (key.depth && occlusion_open_ && pitch >= 640 && !masks &&
        REXCVAR_GET(bo2zm_native_test_occlusion_always)) {
      key.depth |= 0x7 << 4;  // test: Z function always
    }
    key.rasterization = (mode_sc & 0x7) | (restart ? 0x8 : 0) | (with_bias ? 0x10 : 0);
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Looked up with LookupKey (phase 0a: canonical form; phases 1 and 2: without the state set through
    // vkCmdSet*). key stays raw: the deferred sky (opaque_in_all), the counter and the dynamic state read
    // it.
    CutSubstage(49, t_pipeline);
    BO2ZM_SUB(11, pipeline = PipelineOf(LookupKey(key), *input, p));
    if (pipeline == VK_NULL_HANDLE) {
      return false;
    }
#if REX_PLATFORM_SWITCH
    // The pipeline about to be bound, prefetched into the cache several us before its vkCmdBindPipeline
    // (Mesa patch p06; PRFM only: it reads and writes nothing and cannot fail).
    if (nvk_prefetch_app_ && pipeline != pipeline_bound_ && vk_switch_prefetch_pipeline) {
      // With deferred recording the bind is made by the recorder thread: the prefetch goes to its queue too.
      deferred::Prefetch(vk_switch_prefetch_pipeline, pipeline);
    }
#endif
    CutSubstage(50, t_pipeline);

    Stage(5, mark);
    /*
     * The three dynamic state values are computed here, ahead of the recording block and without recording
     * anything. They are pure computations on the draw's registers, needed this early so they can be saved
     * if this draw is the sky and gets deferred (see below).
     */
    // Scissor test. All the state and the draw are sent as usual; the only change is that there are no
    // pixels to shade. It breaks the image: only for measuring the per-draw floor.
    VkRect2D scissor_final = scissor;
    if (const uint32_t category_scissor = scissor_test_; category_scissor != 0) {
      const uint32_t category = TargetCategory(pitch, keys);
      static constexpr uint32_t kCategoryOf[6] = {0, kGpuShadows, kGpuScene, kGpu320,
                                                   kGpuSceneNoDepth, kGpuReflection};
      if (category_scissor <= 5 && category == kCategoryOf[category_scissor]) {
        scissor_final.extent = {1, 1};
      }
    }
    /*
     * Record how far down the game draws in this render target. It only grows, so the pass is never opened
     * smaller than what has already been seen drawn.
     */
    if (area_util_) {
      const uint32_t until = uint32_t(std::max(0, scissor.offset.y + int32_t(scissor.extent.height)));
      const uint32_t rounded = (until + 63u) & ~63u;
      uint32_t& pointed = HeightUsefulOf(pitch);  // without a map lookup on every draw
      if (rounded > pointed) {
        pointed = rounded;
      }
    }
    const float blend_constant[4] = {Float(r[gr::XE_GPU_REG_RB_BLEND_RED]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_GREEN]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_BLUE]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_ALPHA])};
    const uint32_t stencil_frontface = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    const uint32_t stencil_backface = ((control_depth >> 7) & 0x1)
                                       ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]
                                       : stencil_frontface;
    /*
     * The sky, deferred until after the opaque draws (bo2zm_native_postponed_sky).
     *
     * How it is recognized, and why it cannot be mistaken for another draw. Three conditions at once, none of
     * them a magic position or draw count:
     *
     *  1. The pixel shader is the sky's, by the fingerprint of its original container (kHashSky). It is
     *     the only one of the library's 152 shaders whose constant table names CloudIntensity and
     *     SkyAlphaTag. The fingerprint does not depend on the SPIR-V translation or on the library order.
     *  2. The draw writes color and is opaque on all its targets: blending is "1 x source + 0 x destination"
     *     (ADD) on the channels the mask writes, exactly the criterion PipelineOf uses to decide
     *     blendEnable. Opaque = it does not read the existing color, so whatever was painted before it does
     *     not matter.
     *  3. It tests depth and does not write it, and there is no stencil. Without a Z write, deferring it
     *     cannot change what later draws see; with the test on, in its new place the Z test discards it
     *     wherever the world would have painted over it.
     *
     * If any of the three fails (another version of the game, another state) the draw takes the usual path
     * and nothing happens.
     *
     * It is not deferred while a game occlusion query is open: the game reads those samples and moving it
     * would change the count (the same guard the early Z uses).
     */
    // A draw with no color to write does not read the target: it does not count as transparent.
    bool opaque_in_all = true;
    for (uint32_t i = 0; i < 4 && opaque_in_all; ++i) {
      const uint32_t mask_i = (masks >> (i * 4)) & 0xF;
      if (!mask_i) {
        continue;
      }
      const uint32_t m = key.blend[i];
      const bool blend_on = ColorBlendEnabled(mask_i, m);
      if (blend_on) {
        opaque_in_all = false;
      }
    }
    const bool test_z_draw = ((control_depth >> 1) & 0x1) != 0;
    const bool writes_z_draw = test_z_draw && ((control_depth >> 2) & 0x1) != 0 &&
                                  keys[4] != 0;
    bool is_sky = false;
    if (ps && ps->shader && ps->shader->hash == kHashSky && masks) {
      ++sky_seen_;
      const bool has_stencil = (control_depth & 0x1) != 0;
      /*
       * Scene pass only. The same dome is also drawn in the environment cubemap and in the reflection, and there it
       * is not touched: the reflection works and is left alone, and the measured saving is the scene's.
       * This line is what keeps the cubemap out of it.
       */
      const bool in_scene = TargetCategory(pitch, keys) == kGpuScene;
      /*
       * The fingerprint is not enough.
       *
       * The fingerprint identifies the shader, not the draw: in some scenes several draws per frame use that
       * pixel shader ("detected per frame" in the report), and deferring one breaks the order of the others,
       * which shows as flickering and odd colors.
       *
       * So it must also be the dome:
       *   - the index count measured by the analysis of the SPIR-V and the log: 480 = 160 triangles;
       *   - and it must be the first draw of the pass (the dome is painted on the empty Z-buffer, which is
       *     exactly why it shades the whole screen and is then covered).
       *
       * Both are cheap to check and both belong to the dome, not to the shader. If the counter still said
       * more than one per frame, nothing is deferred: there is a guard below.
       */
      const uint32_t indices_draw =
          uint32_t(indices_16 ? indices16_.size() : indices_.size());
      const bool dome_geometry = indices_draw == kIndicesDomeSky;
      const bool first_in_pass = draws_in_pass_ == 0;
      /* The full criterion, without the cvar or the guard: what is measured during the test. */
      const bool is_dome = in_scene && opaque_in_all && test_z_draw &&
                              !writes_z_draw && !has_stencil && !(ps->outputs & 0x10) &&
                              !occlusion_open_ && dome_geometry && first_in_pass;
      if (is_dome) {
        ++sky_candidates_;  // the ones matching the dome's marks: this must be 1
        /*
         * The guard counts here, whether it defers or not. Two in the same frame is exactly the case that
         * breaks the image, so as soon as it happens it switches off and stays off for the rest of the run.
         */
        if (++sky_guard_in_frame_ > 1 && sky_guard_ != kSkyDiscarded) {
          sky_guard_ = kSkyDiscarded;
          REXLOG_WARN("[native] C6 sky: the guard sees {} domes in the SAME frame; nothing is postponed for the whole "
                      "session (a known bug of older builds). The image is untouched",
                      sky_guard_in_frame_);
        }
      }
      is_sky = sky_postpone_on_ && is_dome && sky_guard_ == kSkyPostponing;
      if (!is_sky && sky_postpone_on_) {
        ++sky_no_postponable_;  // only with the setting on: measures criterion failures, not the cvar
      }
    }
    /*
     * Which draws the sky can be moved past, and why that does not change a single pixel.
     *
     * The sky can only skip past a draw D if both hold:
     *
     *  - D is opaque: it does not read the existing color, so wherever D paints, the final color is its own
     *    whether or not the sky was underneath. A draw with blending does read the background: the sky goes
     *    first.
     *  - D writes depth: after D the Z-buffer holds D's z, which is closer than the dome, so the sky's LEQUAL
     *    test discards it exactly where D painted. If D did not write Z, the Z-buffer would stay as it was
     *    and the sky would be painted over D: that would change the image.
     *
     * So the sky is emitted as soon as the first draw that fails either condition arrives. In practice that
     * is the first transparent draw, because what follows the sky is the opaque world; the counter separates
     * the two reasons so this can be checked in the log.
     */
    if (sky_pending_) {
      if (is_sky) {
        ++sky_two_in_pass_;  // another sky in the same pass: emit the earlier one now, keeping the order
        EmitSkyPostponed(cmd, kSkyPerOtherSky);
      } else if (!(opaque_in_all && writes_z_draw)) {
        EmitSkyPostponed(cmd, opaque_in_all ? kSkyPerNoZ : kSkyPerBlend);
      }
    }
    // --- Record ---------------------------------------------------------------------------------
    if (is_sky && reads_reflection) {
      bo2zm::reflection_demand::AnnotateVisible(false);  // the deferred sky is not measured
    }
    if (is_sky) {
      // The arguments of the vkCmd* calls this draw would have emitted are saved; nothing is recorded. The
      // upload buffer is an allocator that only moves forward (Reserve) and is not reset until UseSlot,
      // which always comes after BeforeSubmit -> FinishPass: when the sky is emitted, its offsets still
      // point to the same data.
      PostponedSky& c = sky_;
      c.pipeline = pipeline;
      c.buffer = upload_;
      c.uses_ubo = use_ubo_;
      c.push[0] = upload_address_ + constants_vs_offset_;
      c.push[1] = upload_address_ + constants_ps_offset_;
      c.push[2] = upload_address_ + offset_shared;
      c.offsets_ubo = use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_),
                                                          uint32_t(constants_ps_offset_),
                                                          uint32_t(offset_shared)}
                                : std::array<uint32_t, 3>{0, 0, 0};
      c.slot_ubo = slot_current_;
      c.viewport = viewport;
      c.scissor = scissor_final;
      std::memcpy(c.blend, blend_constant, sizeof(c.blend));
      std::memcpy(c.bias, bias, sizeof(c.bias));
      c.with_stencil = keys[4] != 0;
      c.stencil[0] = stencil_frontface;
      c.stencil[1] = stencil_backface;
      c.n_bindings = uint32_t(input->bindings.size());
      c.offsets_vertices = offsets_vertices;
      c.with_indices = with_indices;
      c.indices_16 = indices_16;
      c.indices = uint32_t(indices_16 ? indices16_.size() : indices_.size());
      {
        VkDeviceSize at_indices = offset_indices;
        ResolveVertexBinding(offset_indices, upload_, c.index_buffer, at_indices);  // index cache
        c.first_index = uint32_t(at_indices / (indices_16 ? 2 : 4));
      }
      c.vmin = vmin;
      c.count = count;
      c.ps_more_one = ps ? ps->number + 1 : 0;
      c.category = TargetCategory(pitch, keys);
      c.draws_on_postpone = draws_in_pass_;
      c.eds_mode = eds_mode_;  // phases 1 and 2: its pipeline lacks this state, so save it all
      if (eds_mode_) {
        StateEdsOf(key, c.eds, eds_mode_);
      }
      sky_pending_ = true;
      ++skies_postponed_;
      // Counted as if it had been recorded: it will be recorded before the pass closes.
      ++draws_per_category_[c.category];
      triangles_per_category_[c.category] += (with_indices ? c.indices : count) / 3;
      Stage(6, mark);
      ++drawn_;
      // draws_in_pass_ is not incremented here: this draw is not recorded yet. It is incremented in
      // EmitSkyPostponed, and the difference with draws_on_postpone gives the draws that went ahead of
      // it, which is exactly the geometry that now covers it.
      drawn_timed_ += time_ ? 1 : 0;
      return true;
    }
    if (pipeline != pipeline_bound_) {
      if (count_changes_pipeline_) {  // Measurement only (bo2zm_native_count_pipeline_changes)
        CountChangePipeline(key, pipeline_bound_ == VK_NULL_HANDLE);
      }
      BO2ZM_SUB(0, gpu_.BindGraphicsPipeline(cmd, pipeline));
      pipeline_bound_ = pipeline;
    }
    // Phases 1 and 2. The state that is not baked into the pipeline, before the draw. If the guard sees a
    // difference, dynamic state switches off and this same draw is bound with its usual pipeline.
    if (eds_mode_ && !SetStateDynamic(cmd, key)) {
      BO2ZM_SUB(11, pipeline = PipelineOf(LookupKey(key), *input, p));
      if (pipeline == VK_NULL_HANDLE) {
        return false;
      }
      BO2ZM_SUB(0, gpu_.BindGraphicsPipeline(cmd, pipeline));
      pipeline_bound_ = pipeline;
      key_bound_valid_ = false;  // the counter does not classify this bind
    }
    if (!sets_bound_) {
      BO2ZM_SUB(1, gpu_.BindSets(cmd, layout_pipeline_, 0, 4, sets_.data()));
      sets_bound_ = true;
    }
    // Dynamic state and push constants repeat a lot between consecutive draws: they are only recorded if
    // they change (recording: 0.4 us per draw in gameplay).
    const bool recorded = state_recorded_;
    /*
     * With constants through UBOs, the push constants are dead.
     *
     * Every access in shader_common.h is `BO2ZM_UBO ? set 4 : g_PushConstants...`, and BO2ZM_UBO is
     * specialization constant 1<<8, fixed when the pipeline is created: DXC folds the ternary and the shader
     * does not even reference g_PushConstants. Recording them meant recording something nobody reads.
     *
     * And it is not free: on Maxwell B, NVK has no hardware root table (nvk_use_hw_root_table requires
     * Turing), so each vkCmdPushConstants is a LOAD_CONSTANT_BUFFER_OFFSET plus the array, emitted on the
     * spot. The UBO is active in 100 % of the measured submissions.
     */
    if (!use_ubo_) {
      const uint64_t push[3] = {upload_address_ + constants_vs_offset_,
                                upload_address_ + constants_ps_offset_,
                                upload_address_ + offset_shared};
      if (!recorded || std::memcmp(push, push_recorded_, sizeof(push)) != 0) {
        gpu_.PushConstants(cmd, layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(push), push);
        std::memcpy(push_recorded_, push, sizeof(push));
      }
    }
    // Set 4 (dynamic UBOs). Always bound, because the new shaders use it statically: with the real offsets
    // if bo2zm_native_constants_ubo is set, and 0 otherwise. Only recorded when something changes.
    {
      const std::array<uint32_t, 3> offsets_ubo =
          use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_), uint32_t(constants_ps_offset_),
                                              uint32_t(offset_shared)}
                    : std::array<uint32_t, 3>{0, 0, 0};
      ++set4_draws_;  // C6 set 4 report
      if (!ubo_bound_ || slot_ubo_bound_ != slot_current_ || offsets_ubo != offsets_ubo_bound_) {
        // Which offsets change at each bind (measurement only). What NVK saves by differences depends on it:
        // every cbuf whose offset does not change is one rebind less in the Draw.
        if (!ubo_bound_ || slot_ubo_bound_ != slot_current_) {
          ++set4_first_;
        } else {
          ++set4_changes_[(offsets_ubo[0] != offsets_ubo_bound_[0] ? 1u : 0u) |
                          (offsets_ubo[1] != offsets_ubo_bound_[1] ? 2u : 0u) |
                          (offsets_ubo[2] != offsets_ubo_bound_[2] ? 4u : 0u)];
        }
        BO2ZM_SUB(2, gpu_.BindSets(cmd, layout_pipeline_, 4, 1, &sets_ubo_[slot_current_], 3, offsets_ubo.data()));
        offsets_ubo_bound_ = offsets_ubo;
        slot_ubo_bound_ = slot_current_;
        ubo_bound_ = true;
      }
    }
    if (!recorded || std::memcmp(&viewport, &viewport_recorded_, sizeof(viewport)) != 0) {
      BO2ZM_SUB(3, gpu_.SetViewport(cmd, viewport));
      viewport_recorded_ = viewport;
    }
    if (!recorded || std::memcmp(&scissor_final, &scissor_recorded_, sizeof(scissor_final)) != 0) {
      BO2ZM_SUB(4, gpu_.SetScissor(cmd, gpu::FromVk(scissor_final)));
      scissor_recorded_ = scissor_final;
    }
    if (!recorded ||
        std::memcmp(blend_constant, blend_recorded_, sizeof(blend_constant)) != 0) {
      BO2ZM_SUB(5, gpu_.SetBlendConstants(cmd, blend_constant));
      std::memcpy(blend_recorded_, blend_constant, sizeof(blend_constant));
    }
    // Also without bias: the state is dynamic in every pipeline and has to be recorded.
    if (!recorded || std::memcmp(bias, bias_recorded_, sizeof(bias)) != 0) {
      BO2ZM_SUB(6, gpu_.SetDepthBias(cmd, bias[0], bias[1]));
      std::memcpy(bias_recorded_, bias, sizeof(bias));
    }
    if (!recorded) {
      stencil_recorded_valid_ = false;
      type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
    }
    if (keys[4]) {
      const uint32_t frontface = stencil_frontface;  // computed before the recording block
      const uint32_t backface = stencil_backface;
      if (!stencil_recorded_valid_ || frontface != stencil_recorded_[0] ||
          backface != stencil_recorded_[1]) {
        BO2ZM_SUB(7, gpu_.SetStencilValues(cmd, frontface, backface));
        stencil_recorded_[0] = frontface;
        stencil_recorded_[1] = backface;
        stencil_recorded_valid_ = true;
      }
    }
    state_recorded_ = true;
    /*
     * bo2zm_native_zero_base_vertices. With a single binding whose copy starts at a multiple of its stride,
     * the upload buffer is bound at 0 (once per command buffer, or after the deferred sky) and the draw is
     * shifted with first_vertex. The others bind as usual.
     */
    bool base_cero = false;
    uint32_t first_vertex = 0;  // offset / stride: where the copy starts, in vertices
    // Each binding's buffer (the upload buffer or a vertex cache pool) and its offset in it.
    std::array<VkBuffer, 16> bind_buffers;
    std::array<VkDeviceSize, 16> bind_offsets;
    for (size_t i = 0; i < input->bindings.size(); ++i) {
      ResolveVertexBinding(offsets_vertices[i], upload_, bind_buffers[i], bind_offsets[i]);
    }
    if (vertices_base_cero_ && input->bindings.size() == 1) {
      const VkDeviceSize stride = input->bindings[0].stride;
      if (bind_offsets[0] % stride == 0) {
        base_cero = CheckBaseZero(bind_offsets[0], stride, sources[0].bytes, vmin, first_vertex,
                                  bind_buffers[0] == upload_ ? std::min(kSizeUpload, upload_size_real_)
                                                             : vpools_[0].size);
      } else {
        ++base_zero_misaligned_;
      }
    } else if (input->bindings.size() > 1) {
      ++base_zero_several_bindings_;
    }
    if (!input->bindings.empty()) {
      // The buffers come from ResolveVertexBinding (the upload buffer or a vertex cache pool); a
      // binding is the same as the recorded one only with the same buffer and offset.
      /*
       * And do not bind again if it is exactly what is already bound.
       *
       * In NVK each binding is 5 dwords plus an invocation of the MME macro NVK_MME_BIND_VB
       * (nvk_cmd_draw.c:4658), with no redundancy check inside: ~2,800 MME macros per frame. With vertex
       * deduplication 28-33 % of the bindings reuse the same offset, so two consecutive draws of the same
       * mesh give exactly the same offsets.
       */
      // On the base-zero path what gets bound is the whole buffer, from 0.
      static constexpr VkDeviceSize kBindingInZero = 0;
      const VkDeviceSize* a_bind = base_cero ? &kBindingInZero : bind_offsets.data();
      const uint32_t n_bindings = uint32_t(input->bindings.size());
      bool equal = n_bindings == bindings_recorded_;
      for (uint32_t i = 0; equal && i < n_bindings; ++i) {
        equal = a_bind[i] == offsets_recorded_[i] && bind_buffers[i] == buffers_vertices_[i];
      }
      if (!equal) {
        BO2ZM_SUB(8, gpu_.BindVertexBuffers(cmd, n_bindings, bind_buffers.data(), a_bind));
        ++base_zero_bindings_recorded_;
        bindings_recorded_ = n_bindings;
        for (uint32_t i = 0; i < n_bindings; ++i) {
          offsets_recorded_[i] = a_bind[i];
          buffers_vertices_[i] = bind_buffers[i];
        }
      } else {
        ++bindings_saved_;
      }
      ++base_cero_total_;
      base_zero_draws_ += base_cero ? 1 : 0;
    }
    // With a game occlusion query open, this draw counts toward it.
    if (occlusion_open_ && query_occlusion_ == UINT32_MAX) {
      BO2ZM_SUB(13, query_occlusion_ = context_->BeginQueryOcclusion());
    }
    // The first 12, then one every 2 s (up to 150), to follow the sun over the course of a match.
    if (occlusion_open_ && pitch >= 640 && warnings_occlusion_draw_ < 16 &&
        (warnings_occlusion_draw_ < 12 ||
         std::chrono::steady_clock::now() - last_warning_occlusion_draw_ >= std::chrono::seconds(2))) {
      ++warnings_occlusion_draw_;
      last_warning_occlusion_draw_ = std::chrono::steady_clock::now();
      REXLOG_INFO("[native] C2 occlusion draw {}: host query {}: VS n{} PS n{} type {} count {} pitch {} masks {:08X} "
                  "depth {:08X} (write {}, test {}, z function {}) viewport {:.1f},{:.1f} {:.1f}x{:.1f} z {:.3f}-{:.3f} "
                  "scissor {},{} {}x{} with indices {}",
                  warnings_occlusion_draw_, query_occlusion_ == UINT32_MAX ? -1 : int64_t(query_occlusion_),
                  p.vs->number, ps ? int(ps->number) : -1, type, count, pitch, masks, control_depth,
                  (control_depth >> 2) & 0x1, (control_depth >> 1) & 0x1, (control_depth >> 4) & 0x7,
                  viewport.x, viewport.y, viewport.width, viewport.height, viewport.min_depth, viewport.max_depth,
                  scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height, with_indices);
      // Diagnostic: positions of the first vertices and VS constants c0-c6, to compute the sun's z against
      // the scene depth.
      std::string positions;
      for (const VertexAttribute& a : input->attributes) {
        if (a.location != 0) {
          continue;
        }
        const VertexSource& source = sources[a.binding];
        const uint32_t stride = input->bindings[a.binding].stride;
        for (uint32_t v = 0; v < std::min<uint32_t>(count, 4); ++v) {
          const uint64_t since = uint64_t(v) * stride + a.offset;
          if (since + 16 > source.bytes) {
            break;
          }
          positions += " (";
          for (uint32_t k = 0; k < 4; ++k) {
            uint32_t word;
            std::memcpy(&word, source.data + since + k * 4, 4);
            positions += fmt::format("{}{:.7g}", k ? "," : "", Float(xenos::GpuSwap(word, source.order)));
          }
          positions += ")";
        }
        positions += fmt::format(" format {} stride {}", int(a.format), stride);
        break;
      }
      std::string constants;
      for (uint32_t k = 0; k < 7; ++k) {
        constants += fmt::format(" c{}=(", k);
        for (uint32_t c = 0; c < 4; ++c) {
          constants += fmt::format("{}{:.7g}", c ? "," : "", Float(r[kRegConstantsVs + k * 4 + c]));
        }
        constants += ")";
      }
      REXLOG_INFO("[native] C2 occlusion draw {} (geometry): VTE {:08X} CLIP {:08X} sc {:08X} RB_DEPTH_INFO {:08X} pass "
                  "{}x{} ndc ({:.5g},{:.5g},{:.5g},{:.5g}) pipeline z function {} vmin {}; positions{}; VS constants{}",
                  warnings_occlusion_draw_, vte, r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], mode_sc,
                  r[gr::XE_GPU_REG_RB_DEPTH_INFO], pass_width_, pass_height_, ndc[0], ndc[1], ndc[2], ndc[3],
                  (key.depth >> 4) & 0x7, vmin, positions, constants);
    }
    // Draws and triangles per render target type (same classification as the GPU time).
    {
      const uint32_t category = TargetCategory(pitch, keys);
      const uint32_t vertices_draw = with_indices ? uint32_t(indices_16 ? indices16_.size() : indices_.size())
                                                   : count;
      ++draws_per_category_[category];
      triangles_per_category_[category] += vertices_draw / 3;
    }
    // In the diagnostic frame, one query per draw with its pixel shader.
    // The diagnostic window is decided by the context, at the Swap. With the diagnostic off (the normal
    // case) this does not even reach the call.
    const uint32_t query_draw =
        !diag_stats_draw_
            ? UINT32_MAX
            : context_->BeginStatsDraw(
                  ps ? ps->number + 1 : 0,
                  TargetCategory(pitch, keys));
    /*
     * reflection visibility. An occlusion query around this draw alone, if it samples the reflection:
     * 0 samples = it wrote nothing to any target. Two of the same type cannot be open at once, so while a
     * game query is open it is not measured and counts as visible. Every kWitnessEvery frames the final
     * composite, which paints the whole screen, is measured the same way: it must produce samples (the
     * frame-skip code's guard).
     */
    uint32_t query_visibility = UINT32_MAX;
    if (reads_reflection) {
      if (!occlusion_open_ && query_occlusion_ == UINT32_MAX) {
        query_visibility = context_->BeginQueryVisibility(false);
      }
      if (query_visibility == UINT32_MAX) {
        bo2zm::reflection_demand::AnnotateVisible(false);
      }
    } else if (witness_pending_ && ps && ps->shader && ps->shader->hash == kHashComposite &&
               !occlusion_open_ && query_occlusion_ == UINT32_MAX) {
      query_visibility = context_->BeginQueryVisibility(true);
      if (query_visibility != UINT32_MAX) {
        witness_pending_ = false;
        // While the first 8 are checked, one every 2 frames: the guard finishes in ~0.5 s of play.
        witness_next_ =
            frame_ + (bo2zm::reflection_demand::VisibilityChecked() ? kWitnessEvery : uint64_t(2));
      }
    }
    if (with_indices) {
      // The upload buffer is bound once per index type and each draw uses firstIndex.
      const VkIndexType type_indices = indices_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      // Or the index cache's pool (bo2zm_native_index_cache), with the same tag as the vertex cache's.
      VkBuffer buffer_indices = upload_;
      VkDeviceSize at_indices = offset_indices;
      ResolveVertexBinding(offset_indices, upload_, buffer_indices, at_indices);
      if (type_indices != type_indices_recorded_ || buffer_indices != buffer_indices_recorded_) {
        BO2ZM_SUB(9, gpu_.BindIndexBuffer(cmd, buffer_indices, indices_16));
        type_indices_recorded_ = type_indices;
        buffer_indices_recorded_ = buffer_indices;
      }
      // On the base-zero path, vertexOffset also carries where the copy starts (in vertices).
      const int32_t offset_vertices =
          base_cero ? int32_t(int64_t(first_vertex) - int64_t(vmin)) : -int32_t(vmin);
      BO2ZM_SUB(10, gpu_.DrawIndexed(cmd, uint32_t(indices_16 ? indices16_.size() : indices_.size()),
                                     uint32_t(at_indices / (indices_16 ? 2 : 4)), offset_vertices));
    } else {
      BO2ZM_SUB(10, gpu_.Draw(cmd, count, base_cero ? first_vertex : 0));  // firstVertex
    }
    if (query_visibility != UINT32_MAX) {
      context_->FinishQueryVisibility(query_visibility);
    }
    if (query_draw != UINT32_MAX) {
      context_->FinishStatsDraw(query_draw);
    }
    Stage(6, mark);
    if (time_) {
      ++sub_samples_;
      ReportSubstages();
    }
    ++drawn_;
    ++draws_in_pass_;  // to know at which position of the pass the sky ends up emitted
    drawn_timed_ += time_ ? 1 : 0;
    return true;
  }

  /*
   * Emits the deferred sky draw (bo2zm_native_postponed_sky).
   *
   * It records exactly the same vkCmd* calls it would have recorded in its original place, with the
   * values saved then. It relies on nothing recorded afterwards: it sends all of this draw's dynamic
   * state, and when done it invalidates the tracked state so the next draw records its own again. That
   * way the order cannot slip through an "already set" comparison.
   *
   * The upload buffer offsets are still valid because Reserve only moves forward and the buffer is not
   * reset until UseSlot, which always comes after BeforeSubmit -> FinishPass.
   */
  void EmitSkyPostponed(VkCommandBuffer cmd, uint32_t reason) {
    if (!sky_pending_ || cmd == VK_NULL_HANDLE) {
      return;
    }
    sky_pending_ = false;
    const PostponedSky& c = sky_;
    ++sky_emitted_[reason < kSkyReasons ? reason : kSkyReasons - 1];
    const uint64_t position = draws_in_pass_ - c.draws_on_postpone;
    sky_position_sum_ += position;
    sky_position_max_ = std::max(sky_position_max_, position);
    gpu_.BindGraphicsPipeline(cmd, c.pipeline);
    pipeline_bound_ = c.pipeline;
    if (c.eds_mode) {  // phases 1 and 2: all the state its pipeline lacks
      EmitStateDynamic(cmd, c.eds, true, c.eds_mode);
    }
    eds_valid_ = false;  // the next draw sets all of its own again
    key_bound_valid_ = false;  // The sky does not keep its key (CountChangePipeline)
    if (!sets_bound_) {
      gpu_.BindSets(cmd, layout_pipeline_, 0, 4, sets_.data());
      sets_bound_ = true;
    }
    if (!c.uses_ubo) {
      gpu_.PushConstants(cmd, layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(c.push), c.push);
    }
    gpu_.BindSets(cmd, layout_pipeline_, 4, 1, &sets_ubo_[c.slot_ubo], 3, c.offsets_ubo.data());
    gpu_.SetViewport(cmd, c.viewport);
    gpu_.SetScissor(cmd, gpu::FromVk(c.scissor));
    gpu_.SetBlendConstants(cmd, c.blend);
    gpu_.SetDepthBias(cmd, c.bias[0], c.bias[1]);
    if (c.with_stencil) {
      gpu_.SetStencilValues(cmd, c.stencil[0], c.stencil[1]);
    }
    if (c.n_bindings) {
      std::array<VkBuffer, 16> buffers{};
      std::array<VkDeviceSize, 16> offsets{};
      for (uint32_t i = 0; i < c.n_bindings; ++i) {
        ResolveVertexBinding(c.offsets_vertices[i], c.buffer, buffers[i], offsets[i]);  // bo2zm_native_vertex_cache
      }
      gpu_.BindVertexBuffers(cmd, c.n_bindings, buffers.data(), offsets.data());
      bindings_recorded_ = 0;  // the next draw binds again
    }
    const uint32_t query = !diag_stats_draw_
                                  ? UINT32_MAX
                                  : context_->BeginStatsDraw(c.ps_more_one, c.category);
    if (c.with_indices) {
      gpu_.BindIndexBuffer(cmd, c.index_buffer, c.indices_16);
      buffer_indices_recorded_ = VK_NULL_HANDLE;
      gpu_.DrawIndexed(cmd, c.indices, c.first_index, -int32_t(c.vmin));
    } else {
      gpu_.Draw(cmd, c.count, 0);
    }
    if (query != UINT32_MAX) {
      context_->FinishStatsDraw(query);
    }
    ++draws_in_pass_;
    // Nothing tracked is valid any more: the dynamic state, the bindings and the index type are the sky's.
    // state_recorded_ = false makes the next draw also re-record stencil and indices.
    state_recorded_ = false;
    ubo_bound_ = false;
    bindings_recorded_ = 0;
    stencil_recorded_valid_ = false;
    type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  }

  // Adds the time since the mark to the stage and moves the mark to now.
  /*
   * Measurement only. What takes the time inside the ring's record and pipeline stages (4.1 and 2.5 us per
   * recorded draw): each Vulkan command, PipelineOf and the shared constants block, on the same timed
   * draws (1 in 64). "C6 substages" line every 10 s: us per draw, us per call and calls per timed draw.
   */
  // Its lines, like those of the other ring reports, go to the report thread (BO2ZM_REPORT_RING).
  void ReportSubstages() {
    const auto now = std::chrono::steady_clock::now();
    if (now < sub_next_) {
      return;
    }
    const bool first = sub_next_ == std::chrono::steady_clock::time_point{};
    sub_next_ = now + std::chrono::seconds(10);
    if (first || !sub_samples_) {
      sub_ns_.fill(0);
      sub_n_.fill(0);
      sub_samples_ = 0;
      return;
    }
    static constexpr const char* kNames[51] = {
        "BindPipeline", "sets 0-3", "set 4 (UBO)", "Viewport", "Scissor", "BlendConstants", "DepthBias",
        "Stencil (6)", "BindVertexBuffers", "BindIndexBuffer", "Draw", "PipelineOf", "shared",
        "occlusion", "samplers (texture stage)",
        // The four segments of the "indices" stage (CutSubstage)
        "index stage: targets and discards", "index stage: depth bias",
        "index stage: indices", "index stage: vertices and diagnostics",
        // Parts of "samplers" (not added to the sum): each sampler by its path.
        "(samplers: slot hit)", "(samplers: table hit)", "(samplers: PrepareTexture)",
        // Parts of "PrepareTexture" (PrepareOutcome)
        "(PrepareTexture: before the check)", "(PrepareTexture: skipped by write stamps)",
        "(PrepareTexture: postponed by the budget)", "(PrepareTexture: sample equal)",
        "(PrepareTexture: full hash equal)", "(PrepareTexture: new texture)", "(PrepareTexture: changed, read again)",
        "(PrepareTexture: to the hash thread)", "(PrepareTexture: took a cold texture's image)",
        // Parts of a new texture (CutSubstage in PrepareTexture)
        "(new texture: hash and look up by content)", "(new texture: create the image)", "(new texture: raw hash)",
        "(new texture: read and untile)", "(new texture: byte order, data hash, queue the upload)",
        // Parts of the "uploads" stage
        "(uploads: textures)", "(uploads: vertices)", "(uploads: indices)", "(uploads: constants)",
        // Parts of "uploads: vertices" (per binding) and of "uploads: constants" (per copy)
        "(vertices: same-frame dedupe)", "(vertices: vertex cache)", "(vertices: copied)", "(constants: VS copy)",
        "(constants: PS copy)",
        // A vertex or index cache lookup's parts (VertexCacheByWrites, IndexCacheByWrites)
        "(cache lookup: slot)", "(cache lookup: stamps)", "(cache lookup: guard)",
        // Parts of the "pipeline" stage
        "(pipeline stage: viewport, scissor, shared)", "(pipeline stage: key)", "(pipeline stage: lookup and prefetch)"};
    std::string line;
    double total = 0.0;
    for (size_t k = 0; k < 51; ++k) {
      if (!sub_n_[k]) {
        continue;
      }
      const double per_draw = double(sub_ns_[k]) / 1e3 / double(sub_samples_);
      if (k < 19) {
        total += per_draw;
      }
      line += fmt::format(" | {} {:.2f} us/draw ({:.2f} us x {:.2f} per draw)", kNames[k], per_draw,
                           double(sub_ns_[k]) / 1e3 / double(sub_n_[k]),
                           double(sub_n_[k]) / double(sub_samples_));
    }
    BO2ZM_REPORT_RING("[native] C6 substages ({} timed draws; sum {:.2f} us per draw){}; constants copied: {} KB VS "
                      "and {} KB PS in 10 s",
                      sub_samples_, total, line, bytes_constants_vs_ >> 10, bytes_constants_ps_ >> 10);
    bytes_constants_vs_ = bytes_constants_ps_ = 0;
    sub_ns_.fill(0);
    sub_n_.fill(0);
    sub_samples_ = 0;
  }

  /*
   * The base-zero path of a draw (bo2zm_native_zero_base_vertices). Returns false if it has to bind as
   * usual. Self-checking guard: the first kBaseZeroACheck draws, then 1 in 4,096, redo the computation
   * backwards in 64 bits (the first vertex the GPU reads falls on the first byte of the copy, with and
   * without indices), and check that vertexOffset fits in a signed 32-bit value and that the copy fits in
   * the buffer. On a single difference: that draw binds as usual, MISMATCH in the log and the path is off
   * for the rest of the run.
   */
  bool CheckBaseZero(VkDeviceSize offset, VkDeviceSize stride, uint64_t bytes, uint32_t vmin,
                         uint32_t& first_vertex, VkDeviceSize limit) {
    const VkDeviceSize first = offset / stride;
    if (first > VkDeviceSize(INT32_MAX)) {
      return false;  // does not fit in firstVertex/vertexOffset (cannot happen with a 64 MB buffer)
    }
    const uint64_t n = ++base_zero_checked_;
    if (n <= kBaseZeroACheck || (n & 4095) == 0) {
      const int64_t displacement = int64_t(first) - int64_t(vmin);  // the vertexOffset of indexed draws
      const bool ok = first * stride == offset &&
                        (displacement + int64_t(vmin)) * int64_t(stride) == int64_t(offset) &&
                        displacement >= int64_t(INT32_MIN) && displacement <= int64_t(INT32_MAX) &&
                        offset + bytes <= limit;
      if (!ok) {
        vertices_base_zero_off_ = true;
        vertices_base_cero_ = false;
        REXLOG_ERROR("[native] C6 zero-base vertices: MISMATCH (offset {} stride {} bytes {} vmin {} first {}): this "
                     "draw and the rest of the session bind as always",
                     offset, stride, bytes, vmin, first);
        return false;
      }
      if (n == kBaseZeroACheck) {
        BO2ZM_REPORT_RING("[native] C6 zero-base vertices: {} draws checked, 0 mismatches; still checking 1 in 4096", n);
      }
    }
    first_vertex = uint32_t(first);
    return true;
  }

  /*
   * Set 4 by differences, once per submission (UseSlot). Tells NVK whether it is wanted
   * (bo2zm_native_set4_by_differences and the test alternation) and, if the NVK guard has seen a MISMATCH,
   * reports it in the log as an error once: NVK has already switched it off for the rest of the run.
   */
  void ControlSet4() {
    bool requested = REXCVAR_GET(bo2zm_native_set4_by_differences);
    const int32_t alternate = REXCVAR_GET(bo2zm_native_set4_by_differences_alternate_s);
    if (alternate > 0) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                             set4_start_alternation_)
                                .count();
      requested = (seconds / alternate) % 2 == 1;
    }
#if REX_PLATFORM_SWITCH
    NvkSwitchSet4* const nvk = &nvk_switch_set4;
    const bool with_patch = nvk != nullptr && nvk->version == 1;
#else
    const bool with_patch = false;
#endif
    if (requested != set4_requested_ || !set4_requested_annotated_) {
      set4_requested_ = requested;
      set4_requested_annotated_ = true;
      REXLOG_INFO("[native] C6 set 4 by differences: {} (frame {}){}",
                  requested ? "requested from NVK" : "NOT requested: NVK binds the whole set", frame_,
                  with_patch ? "" : "; this NVK does not have nvk_switch_set4 (older Mesa or PC): binds as always");
    }
#if REX_PLATFORM_SWITCH
    if (with_patch) {
      __atomic_store_n(&nvk->requested, requested ? 1 : 0, __ATOMIC_RELAXED);
      if (!set4_mismatch_warned_ && __atomic_load_n(&nvk->off, __ATOMIC_RELAXED) != 0) {
        set4_mismatch_warned_ = true;
        REXLOG_ERROR("[native] C6 set 4 by differences: MISMATCH seen by NVK's guard ({} cbufs bound wrong, each one "
                     "fixed in its draw; details in rex_stderr.log). Off for the rest of the session: NVK binds the "
                     "whole set, as always",
                     __atomic_load_n(&nvk->total.mismatches, __ATOMIC_RELAXED));
      }
    }
#endif
  }

  /*
   * The draw path in NVK, once per submission (like ControlSet4). Tells NVK what to measure and which
   * improvements are wanted (they apply from the next command buffer) and, if an improvement's guard has
   * seen a MISMATCH, reports it in the log as an error once: NVK has already switched it off for the
   * rest of the run.
   */
  void ControlDrawNvk() {
#if REX_PLATFORM_SWITCH
    NvkSwitchDraw* const nvk = &nvk_switch_draw;
    if (nvk == nullptr || nvk->version != 1) {
      nvk_prefetch_app_ = false;
      if (!nvk_draw_annotated_) {
        nvk_draw_annotated_ = true;
        REXLOG_INFO("[native] C6 NVK per draw: this NVK does not have the nvk_switch_draw contract (older "
                    "Mesa): no measurement and no improvements");
      }
      return;
    }
    int32_t measure = std::max<int32_t>(0, REXCVAR_GET(bo2zm_native_nvk_measure));
    if (measure > 0) {
      measure = int32_t(std::bit_floor(uint32_t(measure)));  // NVK wants a power of 2
    }
    bool improvements = true;
    const int32_t alternate = REXCVAR_GET(bo2zm_native_nvk_alternate_s);
    if (alternate > 0) {
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - nvk_draw_start_)
              .count();
      improvements = (seconds / alternate) % 2 == 1;
    }
    const int32_t requested[5] = {improvements && REXCVAR_GET(bo2zm_native_nvk_emission) ? 1 : 0,
                                improvements && REXCVAR_GET(bo2zm_native_nvk_cbufs) ? 1 : 0,
                                improvements && REXCVAR_GET(bo2zm_native_nvk_dynamic) ? 1 : 0,
                                0,  // the fast set 4 path (p05) is not applied
                                improvements && REXCVAR_GET(bo2zm_native_nvk_prefetch) ? 1 : 0};
    __atomic_store_n(&nvk->measure, measure, __ATOMIC_RELAXED);
    __atomic_store_n(&nvk->measure_misses, REXCVAR_GET(bo2zm_native_nvk_measure_misses) ? 1 : 0, __ATOMIC_RELAXED);
    for (size_t m = 0; m < 5; ++m) {
      __atomic_store_n(&nvk->improvements[m].requested, requested[m], __ATOMIC_RELAXED);
    }
    nvk_prefetch_app_ = requested[4] != 0;
    if (!nvk_draw_annotated_ || improvements != nvk_improvements_requested_) {
      nvk_draw_annotated_ = true;
      nvk_improvements_requested_ = improvements;
      REXLOG_INFO("[native] C6 NVK per draw: improvements {} (emission {}, cbufs {}, dynamic {}, prefetch "
                  "{}); measuring 1 in {} calls{}; environment NVK_SWITCH_DRAW {} (frame {})",
                  improvements ? "requested" : "NOT requested", requested[0], requested[1], requested[2], requested[4], measure,
                  REXCVAR_GET(bo2zm_native_nvk_measure_misses) ? " with the cache misses separately" : "",
                  __atomic_load_n(&nvk->env, __ATOMIC_RELAXED), frame_);
    }
    static constexpr const char* kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "prefetch"};
    for (size_t m = 0; m < 5; ++m) {
      if (!nvk_off_warned_[m] && __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0) {
        nvk_off_warned_[m] = true;
        REXLOG_ERROR("[native] C6 NVK per draw: MISMATCH seen by the guard of '{}' ({} checks that differed; details in "
                     "rex_stderr.log). Off for the rest of the session: NVK does it as always",
                     kNames[m], __atomic_load_n(&nvk->improvements[m].mismatches, __ATOMIC_RELAXED));
      }
    }
#endif
  }

  /*
   * Every 10 s, the NVK figures. Per part, us per measured call and us per measured draw; per measured
   * draw, how much work it carries; per pipeline bind, how many state copies changed nothing; and per
   * improvement, uses, checks and differences. Totals are from command buffers that have already finished.
   */
  void ReportDrawNvk() {
#if REX_PLATFORM_SWITCH
    const auto now = std::chrono::steady_clock::now();
    if (now - nvk_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = nvk_report_ == std::chrono::steady_clock::time_point{};
    nvk_report_ = now;
    NvkSwitchDraw* const nvk = &nvk_switch_draw;
    if (nvk == nullptr || nvk->version != 1) {
      return;
    }
    uint64_t times[16], ticks[16], counts[13], improvements[5][4];
    for (size_t i = 0; i < 16; ++i) {
      const uint64_t v = __atomic_load_n(&nvk->parts[i].times, __ATOMIC_RELAXED);
      const uint64_t t = __atomic_load_n(&nvk->parts[i].ticks, __ATOMIC_RELAXED);
      times[i] = v - nvk_partes_previous_[i][0];
      ticks[i] = t - nvk_partes_previous_[i][1];
      nvk_partes_previous_[i][0] = v;
      nvk_partes_previous_[i][1] = t;
    }
    for (size_t i = 0; i < 13; ++i) {
      const uint64_t c = __atomic_load_n(&nvk->counts[i], __ATOMIC_RELAXED);
      counts[i] = c - nvk_counts_previous_[i];
      nvk_counts_previous_[i] = c;
    }
    for (size_t m = 0; m < 5; ++m) {
      const uint64_t c[4] = {__atomic_load_n(&nvk->improvements[m].uses, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].validated, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].mismatches, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].no_check, __ATOMIC_RELAXED)};
      for (size_t k = 0; k < 4; ++k) {
        improvements[m][k] = c[k] - nvk_improvements_previous_[m][k];
        nvk_improvements_previous_[m][k] = c[k];
      }
    }
    if (first) {
      return;
    }
    const uint64_t tps = __atomic_load_n(&nvk->ticks_per_second, __ATOMIC_RELAXED);
    const double us_tick = tps ? 1e6 / double(tps) : 0.0;
    const uint64_t draws = counts[0];  // NVK_SW_C_DRAWS_MEASURED
    // us per measured call and, in brackets, us per measured draw.
    const auto part = [&](size_t i) {
      return fmt::format("{:.2f} [{:.2f}]", times[i] ? double(ticks[i]) * us_tick / double(times[i]) : 0.0,
                         draws ? double(ticks[i]) * us_tick / double(draws) : 0.0);
    };
    const auto per_draw = [&](size_t i) { return draws ? double(counts[i]) / double(draws) : 0.0; };
    if (draws) {
      BO2ZM_REPORT_RING(
          "[native] C6 NVK by parts (us per measured call [us per measured draw]; {} draws measured): whole "
          "draw {} | push desc {} | dynamic {} | shader touch {} | shaders {} | cbufs {} | emit {} | BindPipeline: "
          "shaders {} touch {} state copy {} | sets {} (root {} dirtying {}) | root table {} | new chunk {} | "
          "BindVertexBuffers {}",
          draws, part(0), part(1), part(2), part(3), part(4), part(5), part(6), part(7), part(8),
          part(9), part(10), part(11), part(12), part(13), part(14), part(15));
      BO2ZM_REPORT_RING(
          "[native] C6 NVK per measured draw: {:.1f} dwords; {:.2f} with dirty dynamic state ({:.1f} bits); {:.2f} with "
          "dirty shaders; {:.2f} cbufs rebound | pipelines: {} binds measured, {:.1f} % copies that changed nothing, "
          "{:.2f} new bits per bind | {} root table uploads ({:.1f} words each), {} new chunks",
          per_draw(1), per_draw(2), counts[2] ? double(counts[3]) / double(counts[2]) : 0.0, per_draw(4),
          per_draw(5), counts[6], counts[6] ? 100.0 * double(counts[7]) / double(counts[6]) : 0.0,
          counts[6] ? double(counts[8]) / double(counts[6]) : 0.0, counts[10],
          counts[10] ? double(counts[11]) / double(counts[10]) : 0.0, counts[12]);
    }
    static constexpr size_t kShown[4] = {0, 1, 2, 4};  // without the fast set 4 (p05), which is not applied
    static constexpr const char* kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "prefetch"};
    std::string text;
    for (const size_t m : kShown) {
      const char* state = __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0 ? "OFF by the guard"
                           : __atomic_load_n(&nvk->improvements[m].requested, __ATOMIC_RELAXED) == 0 ? "not requested"
                                                                                              : "on";
      text += fmt::format(" | {} ({}): {} uses, {} checked equal, {} different, {} not checked", kNames[m],
                           state, improvements[m][0], improvements[m][1], improvements[m][2], improvements[m][3]);
    }
    BO2ZM_REPORT_RING("[native] C6 NVK improvements (last 10 s){}", text);
#endif
  }

  /*
   * The two NVK copy changes of patches/mesa-bo2zm.patch, once per submission like ControlDrawNvk. What is
   * wanted goes to Mesa (it applies from the next bind or copy); a MISMATCH seen by the fingerprint guard is reported
   * once as an error (Mesa has already switched it off for the rest of the run).
   */
  void ControlCopiesNvk() {
#if REX_PLATFORM_SWITCH
    VkBo2StateCopy* const state = &vk_bo2_copy_state;
    NvkBo2Copies* const copies = &nvk_bo2_copies;
    const bool with_state = state != nullptr && state->version == 1;
    const bool with_copies = copies != nullptr && copies->version == 1;
    const bool hashes = REXCVAR_GET(bo2zm_native_nvk_fingerprint_copy);
    int32_t engine = std::clamp(REXCVAR_GET(bo2zm_native_nvk_copy_engine), 0, 3);
    const int32_t engine_b = REXCVAR_GET(bo2zm_native_nvk_copy_engine_b);
    if (engine_b >= 0) {
      const int arm = bo2zm::benchmark::AbArm();  // bo2zm_benchmark_ab_s, factor 2
      if (arm >= 0 && (arm & 2)) {
        engine = engine_b;
      }
    }
    if (with_state) {
      __atomic_store_n(&state->requested, hashes ? 1 : 0, __ATOMIC_RELAXED);
    }
    if (with_copies) {
      __atomic_store_n(&copies->engine, engine, __ATOMIC_RELAXED);
    }
    if (!copies_nvk_annotated_ || hashes != copies_nvk_hashes_ || engine != copies_nvk_engine_) {
      copies_nvk_annotated_ = true;
      copies_nvk_hashes_ = hashes;
      copies_nvk_engine_ = engine;
      REXLOG_INFO("[native] C6 NVK changes: state copy by fingerprints {}{}; copy engine: {}{} (frame {})",
                  hashes ? "requested" : "NOT requested",
                  with_state ? "" : " (this Mesa does not have it)",
                  engine == 0   ? "nothing (all through vk_meta)"
                  : engine == 1 ? "texture uploads"
                  : engine == 2 ? "image-to-image copies"
                               : "uploads and image-to-image copies",
                  with_copies ? "" : " (this NVK does not have it)", frame_);
    }
    if (with_state && !copies_nvk_mismatch_warned_ && __atomic_load_n(&state->off, __ATOMIC_RELAXED) != 0) {
      copies_nvk_mismatch_warned_ = true;
      REXLOG_ERROR("[native] C6 NVK state copy by fingerprints: MISMATCH seen by Mesa's guard ({} binds different from "
                   "the usual copy, which is the one kept; details in rex_stderr.log). Off for the rest of the session",
                   __atomic_load_n(&state->mismatches, __ATOMIC_RELAXED));
    }
#endif
  }

  /*
   * Kind views (bo2zm_native_nvk_kind_views), once per submission like ControlCopiesNvk: Mesa applies the
   * switch to the next vkBindImageMemory (on the texture binds thread or here). With bo2zm_native_nvk_kind_views_ab,
   * the benchmark A/B's factor 1. Every 10 s, what it did.
   */
  void ControlKindViewsNvk() {
#if REX_PLATFORM_SWITCH
    NvkBo2KindViews* const views = &nvk_bo2_kind_views;
    const bool with_views = views != nullptr && views->version == 1;
    bool on = REXCVAR_GET(bo2zm_native_nvk_kind_views);
    if (REXCVAR_GET(bo2zm_native_nvk_kind_views_ab)) {
      const int arm = bo2zm::benchmark::AbArm();
      if (arm >= 0) {
        on = (arm & 1) != 0;
      }
    }
    if (with_views) {
      __atomic_store_n(&views->enabled, on ? 1 : 0, __ATOMIC_RELAXED);
    }
    if (!kind_views_annotated_ || on != kind_views_on_) {
      kind_views_annotated_ = true;
      kind_views_on_ = on;
      REXLOG_INFO("[native] C3 NVK kind views (bo2zm_native_nvk_kind_views): {}{} (frame {})",
                  on ? "on, pool textures share one mapping per block" : "off, a mapping per texture",
                  with_views ? "" : " (this NVK does not have them)", frame_);
    }
    const auto now = std::chrono::steady_clock::now();
    if (!with_views || now - kind_views_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = kind_views_report_ == std::chrono::steady_clock::time_point{};
    kind_views_report_ = now;
    uint64_t d[6] = {};
    const uint64_t* const v = &views->views_created;
    for (size_t i = 0; i < 6; ++i) {
      const uint64_t x = __atomic_load_n(v + i, __ATOMIC_RELAXED);
      d[i] = x - kind_views_previous_[i];
      kind_views_previous_[i] = x;
    }
    if (first) {
      return;
    }
    BO2ZM_REPORT_RING(
        "[native] C3 NVK kind views ({}), last 10 s: {} texture planes bound through a shared mapping, {} with a "
        "mapping of their own, {} without a view (failures); {} shared mappings made and {} freed, {} live ({:.0f} MB "
        "of GPU addresses)",
        on ? "on" : "off", d[3], d[4], d[5], d[0], d[1], kind_views_previous_[0] - kind_views_previous_[1],
        double(kind_views_previous_[2]) / (1024.0 * 1024.0));
#endif
  }

  // Every 10 s, what the two changes did since the previous report.
  void ReportCopiesNvk() {
#if REX_PLATFORM_SWITCH
    const auto now = std::chrono::steady_clock::now();
    if (now - copies_nvk_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = copies_nvk_report_ == std::chrono::steady_clock::time_point{};
    copies_nvk_report_ = now;
    VkBo2StateCopy* const state = &vk_bo2_copy_state;
    NvkBo2Copies* const copies = &nvk_bo2_copies;
    uint64_t e[7] = {};
    uint64_t c[6] = {};
    if (state != nullptr && state->version == 1) {
      const uint64_t* const v = &state->copies;
      for (size_t i = 0; i < 7; ++i) {
        const uint64_t x = __atomic_load_n(v + i, __ATOMIC_RELAXED);
        e[i] = x - copies_nvk_state_previous_[i];
        copies_nvk_state_previous_[i] = x;
      }
    }
    if (copies != nullptr && copies->version == 1) {
      const uint64_t* const v = &copies->uploads_engine;
      for (size_t i = 0; i < 6; ++i) {
        const uint64_t x = __atomic_load_n(v + i, __ATOMIC_RELAXED);
        c[i] = x - copies_nvk_copies_previous_[i];
        copies_nvk_copies_previous_[i] = x;
      }
    }
    if (first) {
      return;
    }
    const uint64_t bits = e[2] + e[3];
    BO2ZM_REPORT_RING(
        "[native] C6 NVK changes, last 10 s | state copy by fingerprints ({}): {} binds, {} of pipelines without "
        "fingerprints; {:.2f} bits copied and {:.2f} skipped per bind ({:.1f} % skipped); guard {} checked, {} "
        "different; {} pipelines with fingerprints | copy engine: {} uploads ({} regions), through vk_meta {} uploads "
        "({} regions); image copies {} on the engine, {} through vk_meta",
        state == nullptr || state->version != 1         ? "not in this Mesa"
        : __atomic_load_n(&state->off, __ATOMIC_RELAXED) ? "OFF by the guard"
        : __atomic_load_n(&state->requested, __ATOMIC_RELAXED)  ? "on"
                                                              : "not requested",
        e[0], e[1], e[0] ? double(e[2]) / double(e[0]) : 0.0, e[0] ? double(e[3]) / double(e[0]) : 0.0,
        bits ? 100.0 * double(e[3]) / double(bits) : 0.0, e[4], e[5], e[6], c[0], c[1], c[2], c[3], c[4], c[5]);
#endif
  }

  /*
   * Every 10 s, set 4. What changes at each bind (measured here, with or without the NVK patch) and, if
   * NVK has it, what is saved: root table writes per bind (4 without it), cbufs not rebound and the state
   * of its guard.
   */
  void ReportSet4() {
    const auto now = std::chrono::steady_clock::now();
    if (now - set4_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = set4_report_ == std::chrono::steady_clock::time_point{};
    set4_report_ = now;
    uint64_t bindings = set4_first_;
    for (const uint64_t n : set4_changes_) {
      bindings += n;
    }
    if (!first && set4_draws_ && bindings) {
      const auto pct = [bindings](uint64_t n) { return 100.0 * double(n) / double(bindings); };
      BO2ZM_REPORT_RING(
          "[native] C6 set 4: {:.2f} binds per draw ({} in {} draws); only VS changes {:.1f} %, only PS {:.1f} %, only "
          "shared {:.1f} %, VS+PS {:.1f} %, VS+shared {:.1f} %, PS+shared {:.1f} %, all three {:.1f} %, after buffer, "
          "slot or sky {:.1f} % | constants uploaded again: VS {} by generation, {} by epoch and {} because the shader "
          "reads more; PS {}, {} and {}",
          double(bindings) / double(set4_draws_), bindings, set4_draws_, pct(set4_changes_[1]),
          pct(set4_changes_[2]), pct(set4_changes_[4]), pct(set4_changes_[3]), pct(set4_changes_[5]),
          pct(set4_changes_[6]), pct(set4_changes_[7]), pct(set4_first_), reuploads_vs_[0], reuploads_vs_[1],
          reuploads_vs_[2], reuploads_ps_[0], reuploads_ps_[1], reuploads_ps_[2]);
#if REX_PLATFORM_SWITCH
      if (NvkSwitchSet4* const nvk = &nvk_switch_set4; nvk != nullptr && nvk->version == 1) {
        const uint64_t counts[9] = {__atomic_load_n(&nvk->total.bindings_mismatch, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.bindings_full, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.writes_root, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.dwords_root, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_dirty, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_saved, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.draws, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.checks, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.mismatches, __ATOMIC_RELAXED)};
        uint64_t d[9];
        for (size_t i = 0; i < 9; ++i) {
          d[i] = counts[i] - set4_nvk_previous_[i];
          set4_nvk_previous_[i] = counts[i];
        }
        const char* const state = __atomic_load_n(&nvk->off, __ATOMIC_RELAXED) != 0 ? "OFF by the guard"
                                   : __atomic_load_n(&nvk->env, __ATOMIC_RELAXED) == 0
                                       ? "off by NVK_SWITCH_DYN_UBO_DELTA"
                                   : !set4_requested_ ? "off by the cvar"
                                                   : "on";
        BO2ZM_REPORT_RING(
            "[native] C6 set 4 by differences (NVK, {}): {} binds by differences and {} whole; {:.2f} root table writes "
            "per bind (4 before) with {:.2f} words; cbufs dirtied {} and not dirtied {} ({:.1f} % fewer); guard: {} "
            "cbufs checked ({} draws on the new path), {} mismatches",
            state, d[0], d[1], d[0] ? double(d[2]) / double(d[0]) : 0.0, d[0] ? double(d[3]) / double(d[0]) : 0.0,
            d[4], d[5], d[4] + d[5] ? 100.0 * double(d[5]) / double(d[4] + d[5]) : 0.0, d[7], d[6], d[8]);
      }
#endif
    }
    set4_changes_.fill(0);
    set4_first_ = 0;
    set4_draws_ = 0;
    reuploads_vs_.fill(0);
    reuploads_ps_.fill(0);
  }

  // Every 10 s, how many draws go without binding their own offset (base zero).
  void ReportBaseZero() {
    const auto now = std::chrono::steady_clock::now();
    if (now - base_zero_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = base_zero_report_ == std::chrono::steady_clock::time_point{};
    base_zero_report_ = now;
    if (!first && base_cero_total_) {
      BO2ZM_REPORT_RING("[native] C6 zero-base vertices ({}): {} of {} draws with vertices without binding their own "
                  "offset ({:.1f} %); {} with several bindings and {} with the dedupe copy off a multiple of the "
                  "stride; {:.2f} vkCmdBindVertexBuffers per draw",
                  vertices_base_zero_off_ ? "OFF by the guard"
                  : vertices_base_cero_       ? "on"
                                              : "off",
                  base_zero_draws_, base_cero_total_, 100.0 * double(base_zero_draws_) / double(base_cero_total_),
                  base_zero_several_bindings_, base_zero_misaligned_,
                  double(base_zero_bindings_recorded_) / double(base_cero_total_));
    }
    base_zero_draws_ = 0;
    base_cero_total_ = 0;
    base_zero_several_bindings_ = 0;
    base_zero_misaligned_ = 0;
    base_zero_bindings_recorded_ = 0;
  }

  /*
   * The framing of a draw, kept apart from Draw so it can be cached (bo2zm_native_framing_cache). Returns 0
   * if it has to draw, 1 if the viewport is empty and 2 if the scissor is empty (in both cases Draw returns
   * without drawing; the scissor is left at zero). Same operations in the same order as inline. The only
   * multiplications GCC could fuse with an add or subtract (width * 0.5 and height * 0.5) are exact, so
   * fused or not they give the same bits.
   */
  uint32_t ComputeFraming(const uint32_t* r, uint32_t vte, bool no_clip, uint32_t mode_sc, gpu::Viewport& viewport,
                            float ndc[4], VkRect2D& scissor) {
    float scale_x = (vte & 0x1) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
    float center_x = (vte & 0x2) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
    float scale_y = (vte & 0x4) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    float center_y = (vte & 0x8) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
    const float scale_z = (vte & 0x10) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float center_z = (vte & 0x20) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
    const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    const int32_t window_x = int32_t((window & 0x7FFF) << 17) >> 17;
    const int32_t window_y = int32_t(((window >> 16) & 0x7FFF) << 17) >> 17;
    if ((mode_sc >> 16) & 0x1) {
      center_x += float(window_x);
      center_y += float(window_y);
    }
    // With the shadow map drawn smaller (bo2zm_native_shadow_scale), the guest still speaks in
    // the full map's pixels. The viewport and the scissor are multiplied by the pass scale, and with that the
    // geometry lands where it should; the rest (ndc, half pixel) already comes from the pass size.
    if (pass_scale_ != 1.0f) {
      scale_x *= pass_scale_;
      center_x *= pass_scale_;
      scale_y *= pass_scale_;
      center_y *= pass_scale_;
    }
    viewport = gpu::Viewport{};
    viewport.x = center_x - std::abs(scale_x);
    viewport.width = 2.0f * std::abs(scale_x);
    // A negative YSCALE (the D3D norm) gives a positive height; a positive one, an inverted height.
    viewport.y = center_y + scale_y;
    viewport.height = -2.0f * scale_y;
    if (!((r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 19) & 0x1)) {
      Warn(14, "OpenGL clip space (dx_clip_space_def = 0): Z not adjusted");
    }
    viewport.min_depth = std::clamp(center_z, 0.0f, 1.0f);
    viewport.max_depth = std::clamp(center_z + scale_z, 0.0f, 1.0f);
    // g_NdcScale (x, y) and g_NdcOffset (x, y): identity when clipping is on.
    ndc[0] = 1.0f;
    ndc[1] = 1.0f;
    ndc[2] = 0.0f;
    ndc[3] = 0.0f;
    if (no_clip) {
      const float width = float(pass_width_);
      const float height = float(pass_height_);
      ndc[0] = scale_x * 2.0f / width;
      ndc[2] = (center_x - width * 0.5f) * 2.0f / width;
      // DXC flips Y at the end of the VS (-fvk-invert-y): the D3D Y goes here.
      ndc[1] = -scale_y * 2.0f / height;
      ndc[3] = -(center_y - height * 0.5f) * 2.0f / height;
      viewport.x = 0.0f;
      viewport.y = 0.0f;
      viewport.width = width;
      viewport.height = height;
    }
    scissor = VkRect2D{};
    if (viewport.width < 1.0f || std::abs(viewport.height) < 1.0f) {
      return 1;
    }
    const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
    const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
    int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!((tl >> 31) & 0x1)) {
      x0 += window_x;
      y0 += window_y;
      x1 += window_x;
      y1 += window_y;
    }
    if (pass_scale_ != 1.0f) {  // also in guest pixels
      x0 = int32_t(std::floor(float(x0) * pass_scale_));
      y0 = int32_t(std::floor(float(y0) * pass_scale_));
      x1 = int32_t(std::ceil(float(x1) * pass_scale_));
      y1 = int32_t(std::ceil(float(y1) * pass_scale_));
    }
    x0 = std::clamp(x0, 0, int32_t(pass_width_));
    x1 = std::clamp(x1, 0, int32_t(pass_width_));
    y0 = std::clamp(y0, 0, int32_t(pass_height_));
    y1 = std::clamp(y1, 0, int32_t(pass_height_));
    if (x1 <= x0 || y1 <= y0) {
      return 2;
    }
    scissor = VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    return 0;
  }

  // The C1 guard. The current computation against the saved one, byte by byte.
  void CompareFraming(const gpu::Viewport& viewport, const float ndc[4], const VkRect2D& scissor, uint32_t empty) {
    ++framing_checked_;
    const bool equal = empty == framing_empty_ &&
                       std::memcmp(&viewport, &framing_viewport_, sizeof(viewport)) == 0 &&
                       std::memcmp(ndc, framing_ndc_, sizeof(framing_ndc_)) == 0 &&
                       std::memcmp(&scissor, &framing_scissor_, sizeof(scissor)) == 0;
    if (!equal) {
      framing_cache_off_ = true;
      framing_cache_ = false;
      REXLOG_ERROR("[native] C6 cached framing: MISMATCH in check {} (empty {} / {}; viewport {},{} {}x{} / {},{} "
                   "{}x{}; scissor {},{} {}x{} / {},{} {}x{}). Off for the rest of the session: this draw uses the "
                   "recomputed one",
                   framing_checked_, empty, framing_empty_, viewport.x, viewport.y, viewport.width,
                   viewport.height, framing_viewport_.x, framing_viewport_.y, framing_viewport_.width,
                   framing_viewport_.height, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                   scissor.extent.height, framing_scissor_.offset.x, framing_scissor_.offset.y,
                   framing_scissor_.extent.width, framing_scissor_.extent.height);
    } else if (framing_checked_ == kFramingsACheck) {
      BO2ZM_REPORT_RING("[native] C6 cached framing: {} hits checked against the computation, 0 mismatches; still "
                  "checking 1 in 4096", framing_checked_);
    }
  }

  /*
   * The height_useful_ element for that pitch (bo2zm_native_useful_height_memo). The pitch is the pass's and only
   * changes with the pass, so the last one is remembered. References to unordered_map elements are not
   * invalidated by insertion (only by erasure, and height_useful_ never erases). Guard: the first
   * kHeightUsefulACheck reuses, then 1 in 4,096, also look it up in the map and must get the same element.
   */
  uint32_t& HeightUsefulOf(uint32_t pitch) {
    if (!height_useful_memo_active_) {
      return height_useful_[pitch];
    }
    if (height_useful_memo_ == nullptr || pitch != height_useful_memo_pitch_) {
      height_useful_memo_ = &height_useful_[pitch];
      height_useful_memo_pitch_ = pitch;
      return *height_useful_memo_;
    }
    const uint64_t n = ++height_useful_memo_hits_;
    if (n <= kHeightUsefulACheck || (n & 4095) == 0) {
      uint32_t* const of_map = &height_useful_[pitch];
      if (of_map != height_useful_memo_) {
        height_useful_memo_off_ = true;
        height_useful_memo_active_ = false;
        height_useful_memo_ = nullptr;
        REXLOG_ERROR("[native] C6 remembered useful height: MISMATCH (pitch {}, check {}): it is not the map element. "
                     "Off for the rest of the session", pitch, n);
        return *of_map;
      }
      if (n == kHeightUsefulACheck) {
        BO2ZM_REPORT_RING("[native] C6 remembered useful height: {} checks against the map, 0 mismatches; still "
                    "checking 1 in 4096", n);
      }
    }
    return *height_useful_memo_;
  }

  /*
   * The C3 guard. With the same render target bytes, the XXH3 must be pass_key_. Returns the computed
   * one, which is the one that decides for this draw.
   */
  uint64_t CheckKeyPass(const uint64_t keys[5], uint64_t n) {
    const uint64_t computed = XXH3_64bits(keys, sizeof(uint64_t) * 5);
    if (computed != pass_key_) {
      key_pass_fast_off_ = true;
      key_pass_fast_ = false;
      REXLOG_ERROR("[native] C6 pass key: MISMATCH (check {}: {:016X} computed, {:016X} stored). Off for the rest of "
                   "the session", n, computed, pass_key_);
    } else if (n == kKeysPassACheck) {
      BO2ZM_REPORT_RING("[native] C6 pass key: {} reused ones checked with XXH3, 0 mismatches; still checking 1 in 4096", n);
    }
    return computed;
  }

  // Every 10 s, how much the small per-draw savings save.
  void ReportMinor() {
    const auto now = std::chrono::steady_clock::now();
    if (now - minor_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = minor_report_ == std::chrono::steady_clock::time_point{};
    minor_report_ = now;
    const uint64_t framings = framing_hits_report_ + framing_calcs_report_;
    if (!first && framings) {
      BO2ZM_REPORT_RING("[native] C6 minor: cached framing in {} of {} draws ({:.1f} %; {}{}); pass key "
                  "without XXH3 {} times ({}); remembered useful height {} times ({}); colorless draw settings {}",
                  framing_hits_report_, framings,
                  100.0 * double(framing_hits_report_) / double(framings),
                  framing_cache_off_ ? "OFF by the guard" : framing_cache_ ? "on" : "off",
                  framing_checked_ >= kFramingsACheck ? ", guard passed" : ", checking",
                  keys_pass_fast_ - keys_pass_fast_previous_,
                  key_pass_fast_off_ ? "OFF by the guard" : key_pass_fast_ ? "on" : "off",
                  height_useful_memo_hits_ - height_useful_memo_hits_previous_,
                  height_useful_memo_off_ ? "OFF by the guard" : height_useful_memo_active_ ? "on" : "off",
                  cvars_per_frame_ ? "once per frame" : "on every draw");
    }
    framing_hits_report_ = 0;
    framing_calcs_report_ = 0;
    keys_pass_fast_previous_ = keys_pass_fast_;
    height_useful_memo_hits_previous_ = height_useful_memo_hits_;
  }

  // The guard of the direct-mapped pipeline cache. The same key in the map (the usual path) must give the
  // same VkPipeline as the slot. Otherwise it switches off and returns false.
  bool CheckCellPipeline(uint64_t hash, const PipelineKey& key, VkPipeline in_cell, uint64_t n) {
    ++pipelines_direct_checked_;
    VkPipeline of_map = VK_NULL_HANDLE;
    if (const auto it = pipelines_.find(hash);
        it != pipelines_.end() && std::memcmp(&it->second.first, &key, sizeof(key)) == 0) {
      of_map = it->second.second;
    }
    if (of_map != in_cell) {
      pipelines_direct_off_ = true;
      pipelines_direct_ = false;
      REXLOG_ERROR("[native] C6 pipelines: MISMATCH between the direct cache and the map (check {}, VS {} PS {} "
                   "specialization {:08X}). Off for the rest of the session: the map decides",
                   n, key.vs, key.ps, key.specialization);
      return false;
    }
    if (n == kPipelinesACheck) {
      BO2ZM_REPORT_RING("[native] C6 pipelines: {} direct-cache hits checked against the map, 0 mismatches; still "
                  "checking 1 in 4096", n);
    }
    return true;
  }

  /*
   * Measurement only (bo2zm_native_count_pipeline_changes). What changes at each vkCmdBindPipeline.
   *
   * BindPipeline costs 3.3-4.5 us per call in gameplay (0.3-0.4 per draw), and on each one NVK copies all
   * the pipeline's fixed state (vk_dynamic_graphics_state_copy, ~80 groups). If the change is state only,
   * with dynamic state (vkCmdSet*) no new pipeline would be needed. Before touching anything, the counts
   * and kinds have to be known:
   *  - cull mode, topology within the same class, Z test/write/function, stencil, bias and restart are
   *    EDS1/EDS2, core in Vulkan 1.3: the console (API 1.3.354) already provides them without touching
   *    the SDK;
   *  - blending, color masks and topology of another class need VK_EXT_extended_dynamic_state3, which NVK
   *    exposes on Maxwell and the SDK only enables with ui_vulkan_estado_dinamico3.patch.
   * The new key is compared with the last one bound in the same command buffer, field by field and
   * without XXH3. Blending, masks and depth are compared in canonical form (StateCanonical: only what
   * PipelineOf really reads). If only bits PipelineOf does not use differ, the pipeline is effectively the
   * same: no effect. Cost: an 80-byte memcmp and about 60 operations per bind (0.3-0.4 binds per draw). It
   * changes nothing.
   */
  static uint32_t ClassTopology(uint32_t topology) {
    switch (topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        return 0;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
        return 1;
      case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return 3;
      default:
        return 2;  // triangles: list, strip and fan
    }
  }

  // What PipelineOf really reads from a key's blending, masks and depth, with its same criterion
  // (blendEnable per target; depthWriteEnable = test and write; the back face copies the front).
  static void StateCanonical(const PipelineKey& c, uint32_t blend[4], uint32_t& masks, uint32_t& depth) {
    masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      blend[i] = 0;
      if (!c.formats[i]) {
        continue;  // PipelineOf skips color targets not in the pass
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      if (ColorBlendEnabled(mask, m)) {
        blend[i] = m;  // blendEnable: factors count (without blending Vulkan ignores them)
      }
    }
    uint32_t d = c.formats[4] ? c.depth : 0;  // without a depth target PipelineOf ignores it
    d &= ~0x8u;                                      // PipelineOf does not read bit 3
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // without stencil, its functions and operations do not count
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    depth = d;
  }

  // Classifies a vkCmdBindPipeline from Draw against the last key bound in this buffer.
  // after_begin_pass: BeginPass had forgotten the bound pipeline (pipeline_bound_ set to
  // VK_NULL_HANDLE).
  void CountChangePipeline(const PipelineKey& fresh, bool after_begin_pass) {
    uint64_t* const n = changes_pipeline_.data();
    ++n[kChangeBindings];
    n[kChangeAfterPass] += after_begin_pass ? 1 : 0;
    const PipelineKey& a = key_bound_;
    constexpr uint32_t kBitsTestAlpha = 0x2u | (0x7u << kSpecFunctionAlphaOffset);
    if (!key_bound_valid_) {
      ++n[kChangeFirst];
    } else if (std::memcmp(&a, &fresh, sizeof(fresh)) == 0) {
      ++n[kChangeIdentical];  // only after BeginPass: Vulkan kept the bound pipeline across passes
    } else if (a.vs != fresh.vs || a.ps != fresh.ps) {
      ++n[kChangeShaders];
    } else if (a.input != fresh.input) {
      ++n[kChangeInput];
    } else if (std::memcmp(a.formats, fresh.formats, sizeof(a.formats)) != 0) {
      ++n[kChangeFormats];
    } else if (const uint32_t spec = a.specialization ^ fresh.specialization; spec != 0) {
      ++n[(spec & ~kBitsTestAlpha) == 0                      ? kChangeSpecAlpha
          : (spec & ~(kBitsTestAlpha | kSpecZEarly)) == 0 ? kChangeSpecZEarly
                                                              : kChangeSpecOther];
    } else {
      // Same shaders, input, formats and specialization: only fixed pipeline state changes.
      uint32_t blend_a[4], blend_n[4], masks_a, masks_n, dpth_a, dpth_n;
      StateCanonical(a, blend_a, masks_a, dpth_a);
      StateCanonical(fresh, blend_n, masks_n, dpth_n);
      const bool topology = a.topology != fresh.topology;
      const bool class_changed = ClassTopology(a.topology) != ClassTopology(fresh.topology);
      const bool blend = std::memcmp(blend_a, blend_n, sizeof(blend_a)) != 0;
      const bool masks = masks_a != masks_n;
      const uint32_t dpth = dpth_a ^ dpth_n;
      const uint32_t rast = a.rasterization ^ fresh.rasterization;
      if (!topology && !blend && !masks && !dpth && !rast) {
        ++n[kChangeNoEffect];  // only bits PipelineOf does not read: effectively the same pipeline
        n[kChangeNoEffectAfterPass] += after_begin_pass ? 1 : 0;
      } else {
        const bool eds12 = !blend && !masks && !class_changed;
        ++n[kChangeOnlyState];
        n[kChangeStateAfterPass] += after_begin_pass ? 1 : 0;
        n[kChangeEds12] += eds12 ? 1 : 0;
        n[kChangeEds12AfterPass] += (eds12 && after_begin_pass) ? 1 : 0;
        n[kFieldTopology] += topology ? 1 : 0;
        n[kFieldClassTopology] += class_changed ? 1 : 0;
        n[kFieldBlend] += blend ? 1 : 0;
        n[kFieldMasks] += masks ? 1 : 0;
        n[kFieldZ] += (dpth & 0x76u) ? 1 : 0;
        n[kFieldStencil] += (dpth & ~0x76u) ? 1 : 0;
        n[kFieldFace] += (rast & 0x7u) ? 1 : 0;
        n[kFieldRestart] += (rast & 0x8u) ? 1 : 0;
        n[kFieldBias] += (rast & 0x10u) ? 1 : 0;
      }
    }
    key_bound_ = fresh;
    key_bound_valid_ = true;
  }

  // Every 20 s, what changes at each vkCmdBindPipeline of the ring (measurement only). Reads its cvar on
  // every call: ReportPipelinesDirect calls it, once per frame.
  void ReportChangesPipeline(std::chrono::steady_clock::time_point now) {
    const bool count = REXCVAR_GET(bo2zm_native_count_pipeline_changes);
    if (count != count_changes_pipeline_) {
      count_changes_pipeline_ = count;
      key_bound_valid_ = false;  // what was tracked while not counting cannot be compared
    }
    if (now - changes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = changes_report_ == std::chrono::steady_clock::time_point{};
    const double seconds = std::chrono::duration<double>(now - changes_report_).count();
    changes_report_ = now;
    const uint64_t draws = drawn_ - changes_draws_previous_;
    const uint64_t frames = frame_ - changes_frames_previous_;
    changes_draws_previous_ = drawn_;
    changes_frames_previous_ = frame_;
    const std::array<uint64_t, kChangesN> n = changes_pipeline_;
    changes_pipeline_.fill(0);
    if (first || !count_changes_pipeline_ || !n[kChangeBindings] || !draws || !frames) {
      return;
    }
    const double bindings = double(n[kChangeBindings]);
    const double f = double(frames);
    const auto per_draw = [&](uint64_t x) { return double(x) / double(draws); };
    const auto pct = [&](uint64_t x) { return 100.0 * double(x) / bindings; };
    const uint64_t spec = n[kChangeSpecAlpha] + n[kChangeSpecZEarly] + n[kChangeSpecOther];
    BO2ZM_REPORT_RING(
        "[native] C6 pipeline changes (measurement only): {} vkCmdBindPipeline in {:.0f} s ({:.3f} per draw, "
        "{:.1f} per frame; {} are the first of a pass) | without a previous key {} | the SAME key after a pass begins "
        "{} ({:.1f} %) | shaders change {} ({:.1f} %) | another input {} | other formats {} | specialization {} ({:.1f} "
        "%: alpha test {}, with early Z {}, other {}) | no effect (bits PipelineOf does not read) {} ({:.1f} %) | STATE "
        "ONLY {} ({:.1f} %, {:.3f} per draw): EDS1/EDS2 without touching the SDK {}, the rest needs EDS3",
        n[kChangeBindings], seconds, per_draw(n[kChangeBindings]), bindings / f, n[kChangeAfterPass],
        n[kChangeFirst], n[kChangeIdentical], pct(n[kChangeIdentical]), n[kChangeShaders], pct(n[kChangeShaders]),
        n[kChangeInput], n[kChangeFormats], spec, pct(spec), n[kChangeSpecAlpha], n[kChangeSpecZEarly],
        n[kChangeSpecOther], n[kChangeNoEffect], pct(n[kChangeNoEffect]), n[kChangeOnlyState],
        pct(n[kChangeOnlyState]), per_draw(n[kChangeOnlyState]), n[kChangeEds12]);
    // Avoidable binds per frame with each fix, each one separately (those of the first draw of a pass are
    // not avoided by dynamic state or by the canonical key while BeginPass forgets the bound pipeline).
    constexpr double kUsPerBinding = 3.0;  // BindPipeline: 3.3-4.5 us per call in gameplay
    const uint64_t canonical = n[kChangeNoEffect] - n[kChangeNoEffectAfterPass];
    const uint64_t eds12 = n[kChangeEds12] - n[kChangeEds12AfterPass];
    const uint64_t eds123 = n[kChangeOnlyState] - n[kChangeStateAfterPass];
    BO2ZM_REPORT_RING(
        "[native] C6 pipeline changes, state-only fields: topology {} (of class {}), blend {}, masks {}, Z {}, stencil "
        "{}, cull {}, restart {}, bias {}; {} of them the first of a pass | avoidable binds per frame (at ~3 us each): "
        "{:.1f} with the canonical key ({:.2f} ms), {:.1f} by not forgetting the pipeline when a pass begins ({:.2f} "
        "ms), {:.1f} with EDS1/EDS2 ({:.2f} ms), {:.1f} with EDS1/EDS2/EDS3 ({:.2f} ms)",
        n[kFieldTopology], n[kFieldClassTopology], n[kFieldBlend], n[kFieldMasks], n[kFieldZ],
        n[kFieldStencil], n[kFieldFace], n[kFieldRestart], n[kFieldBias], n[kChangeStateAfterPass],
        double(canonical) / f, double(canonical) / f * kUsPerBinding / 1000.0, double(n[kChangeIdentical]) / f,
        double(n[kChangeIdentical]) / f * kUsPerBinding / 1000.0, double(eds12) / f,
        double(eds12) / f * kUsPerBinding / 1000.0, double(eds123) / f, double(eds123) / f * kUsPerBinding / 1000.0);
  }

  // Every 10 s, the hit rate of the direct-mapped pipeline cache.
  void ReportPipelinesDirect() {
    const auto now = std::chrono::steady_clock::now();
    CollectCompiledPipelines();  // the background compiles that finished
    ReportCompiles(now);
    ReportChangesPipeline(now);  // reads its cvar every frame and writes every 20 s
    TryPrewarm();       // starts the thread as soon as the library is loaded
    ReportPrewarm(now);  // every 10 s, if there is anything new, and its guard
    if (now - pipelines_direct_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = pipelines_direct_report_ == std::chrono::steady_clock::time_point{};
    pipelines_direct_report_ = now;
    const uint64_t hits = pipelines_direct_hits_ - pipelines_direct_hits_previous_;
    const uint64_t misses = pipelines_direct_misses_ - pipelines_direct_misses_previous_;
    pipelines_direct_hits_previous_ = pipelines_direct_hits_;
    pipelines_direct_misses_previous_ = pipelines_direct_misses_;
    if (!first && (hits || misses)) {
      BO2ZM_REPORT_RING("[native] C6 pipelines: direct cache {} hits and {} misses ({:.1f} % of the lookups "
                  "the one-entry shortcut does not resolve); {} checked against the map ({})",
                  hits, misses, 100.0 * double(hits) / double(hits + misses),
                  pipelines_direct_checked_,
                  pipelines_direct_off_ ? "OFF by the guard"
                  : pipelines_direct_checked_ >= kPipelinesACheck ? "guard passed"
                                                                           : "checking");
    }
  }

  /*
   * The "indices" stage once rose from ~1.4 to ~2.6 us per draw without any change to its code. Four
   * cuts inside it (C6 substages 15-18), only on the timed draws (1 in 64): they cost four clock reads on
   * those draws, so the "indices" stage reads ~0.2-0.3 us higher than without them.
   */
  void CutSubstage(size_t k, std::chrono::steady_clock::time_point& t) {
    if (!time_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
    ++sub_n_[k];
    t = now;
  }

  // A sampler's time by path (C6 substages 19-21, parts of "samplers"): 19 slot hit, 20 table hit, 21 PrepareTexture;
  // 22-30 split 21 by what PrepareTexture did (prepare_outcome_, prepare_new_); 31-35 are a new texture's parts;
  // 36-39 the uploads stage's; 40-42 a vertex binding's by path, 43-44 a constant copy's; 45-47 a cache lookup's;
  // 48-50 the pipeline stage's.
  void SamplerPath(uint32_t k, std::chrono::steady_clock::time_point t) {
    if (!time_) {
      return;
    }
    sub_ns_[k] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
    ++sub_n_[k];
  }

  void Stage(size_t stage, std::chrono::steady_clock::time_point& mark) {
    if (!time_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    stages_ns_[stage] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count());
    mark = now;
  }

  // Diagnostic bo2zm_native_diag_repeated_vertices: vertex bytes that repeat address, size, byte order
  // and content within the same frame or relative to an earlier frame.
  void AnnotateVerticesRepeated(uint64_t address, uint32_t bytes, uint32_t order,
                               const uint8_t* data) {
    const auto before = std::chrono::steady_clock::now();
    const uint64_t hash = XXH3_64bits(data, bytes);
    const uint64_t key =
        XXH3_64bits_withSeed(&address, sizeof(address), (uint64_t(bytes) << 2) | order);
    if (vertices_seen_.size() > 200000) {
      vertices_seen_.clear();  // memory cap of the diagnostic
    }
    SeenVertices& seen = vertices_seen_[key];
    if (seen.hash == hash && seen.frame == frame_) {
      bytes_repeated_frame_ += bytes;
    } else if (seen.hash == hash && seen.frame != UINT64_MAX &&
               seen.frame < frame_) {
      bytes_equal_earlier_ += bytes;
    }
    seen.frame = frame_;
    seen.hash = hash;
    ns_hash_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - before)
                                      .count());
  }

  // bo2zm_native_pass_barriers: everything recorded before (passes, copies, clears) finishes and is visible to
  // everything after. Outside a render pass only.
  void PassBarrier(VkCommandBuffer commands) {
    static const bool on = REXCVAR_GET(bo2zm_native_pass_barriers);
    if (!on) {
      return;
    }
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    gpu_.Barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                 nullptr, 0, nullptr);
  }

  void FinishPass() override {
    if (pass_active_) {
      /*
       * Fallback path of the deferred sky. If nothing has emitted it yet (no transparent draw arrived, or the
       * pass closes because of a copy, a resolve or a submission), it is emitted here, inside the pass and
       * before vkCmdEndRenderPass. Losing the sky would be a visible and serious bug, so this is the last
       * place it can be, and it is always reached: the command buffer is only closed through SubmitJob
       * -> BeforeSubmit -> FinishPass.
       */
      if (sky_pending_) {
        EmitSkyPostponed(pass_commands_, kSkyAtPassEnd);
      }
      // A host occlusion query cannot stay open outside its pass.
      if (query_occlusion_ != UINT32_MAX) {
        context_->FinishQueryOcclusion(query_occlusion_);
        query_occlusion_ = UINT32_MAX;
      }
      const auto before_end = std::chrono::steady_clock::now();
      gpu_.EndPass(pass_commands_);
      PassBarrier(pass_commands_);
      if (stats_pass_ != UINT32_MAX) {
        context_->FinishStats(stats_pass_);
        stats_pass_ = UINT32_MAX;
      }
      ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_end).count());
      const uint32_t category_closed = category_pass_;
      pass_active_ = false;
      // Just in case. A pending sky with the pass already closed cannot be recorded; it is counted so it
      // shows up in the report instead of disappearing silently.
      if (sky_pending_) {
        sky_pending_ = false;
        ++sky_lost_;
      }
      // As soon as the shadow pass closes, submit to the GPU. Once per frame: the flag keeps reopened passes
      // from submitting again. SubmitYWait submits and keeps recording in the next slot; it waits for
      // nothing (the name is misleading).
      if (category_closed == kGpuShadows && !submitted_after_shadows_ && context_ &&
          REXCVAR_GET(bo2zm_native_submit_after_shadows)) {
        submitted_after_shadows_ = true;
        ++submits_after_shadows_;
        context_->SubmitYWait();
      }
    }
  }

  void BeforeSubmit() override {
    WaitUploads();  // deferred vertex copies, before flushing the mapping and submitting
    FinishPass();
    // Whatever is still on the texture bind thread, with its views, descriptors, barriers and copies, goes
    // into this upload buffer before it is closed (bo2zm_native_texture_bind_thread). It is the last thing
    // recorded: after the pass closes (which may emit the deferred sky), and meanwhile the thread has kept
    // binding.
    CollectBindings(true);
    // The data of the new textures prepared by the hash thread, into this upload buffer before the mapping
    // is flushed and it is submitted (bo2zm_native_texture_hash_thread). Their barrier and copy are
    // already recorded.
    CollectHashes();
    // GPU skinning. One dispatch for this submission's skinning jobs, and the barrier before its draws
    // (DispatchSkinTable).
    if (skin_on_) {
      DispatchSkinTable();
    }
    if (upload_used_ && !upload_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, upload_memory_, upload_type_, 0,
                                                    upload_size_real_, upload_used_);
    }
    PublishVertexPools();  // bo2zm_native_vertex_cache: the copies made into the pools in this submission
    if (shared_separate_ && shared_used_ && !shared_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, shared_memory_, shared_type_, 0,
                                                    shared_size_real_, shared_used_);
      shared_bytes_published_ += shared_used_;
    }
  }

  void UseSlot(uint32_t slot) override {
    WaitUploads();  // already empty after BeforeSubmit; just in case, before switching buffers
    // GPU skinning. This slot's upload buffer is back from the GPU: its checks are compared before it
    // is filled again; and the new upload command buffer has no pipeline bound yet.
    CheckGpuSkin(uint32_t(slot % uploads_.size()));
    skin_bound_ = false;
    skin_tables_[slot % uploads_.size()] = {};
    ReportCopies();  // every 10 s, who made the vertex copies and how long the ring waited
    ReportCacheFetch();  // every 10 s, the per-fetch sampler table
    // A texture may arrive here with its bind in flight (it was prepared before this submission's first
    // Record) and that is normal; what it cannot have is its data already in the upload buffer just
    // submitted.
    CheckBindingsOnBufferChange();
    CheckHashesOnBufferChange();  // before resetting the upload buffer
    const UploadBuffer& s = uploads_[slot % uploads_.size()];
    upload_ = s.buffer;
    upload_memory_ = s.guest_memory;
    upload_size_real_ = s.size_real;
    upload_data_ = s.data;
    upload_address_ = s.address;
    slot_current_ = uint32_t(slot % uploads_.size());  // this slot's UBO set
    upload_used_ = 0;
    if (shared_separate_) {
      const UploadBuffer& c = shared_bufs_[slot_current_];
      shared_data_ = c.data;
      shared_memory_ = c.guest_memory;
      shared_size_real_ = c.size_real;
      shared_used_ = 0;
    }
    ++epoch_upload_;
    ++frame_;
    bo2zm::page_writes::NextEpoch();  // the vertex cache's write stamps (bo2zm_page_writes.h)
    CloseGuardSkyFrame();
    // reflection visibility, once per submission (the guard may switch it off mid-run).
    measure_visibility_ = bo2zm::reflection_demand::MeasureVisibility();
    if (measure_visibility_ && frame_ >= witness_next_) {
      witness_pending_ = true;
    }
    // The upload buffer is reset here, so the recorded offsets are no longer valid. This also covers
    // SubmitYWait, which goes through here.
    dedupe_.NewFrame(frame_);
    dedupe_indices_.NewFrame(frame_);
    GuardDedupe();
    submitted_after_shadows_ = false;  // the post-shadow submission is once per frame
    /*
     * Diagnostic cvars are read once per frame, not per draw.
     *
     * REXCVAR_GET is not a variable read: it is FLAGS_##name##_storage_(), an out-of-line function call
     * (cvar.h:343). Four of them were being made per draw (one of them per vertex binding, not per draw),
     * and one returned a std::string that was then compared. With 2,345 draws per frame that shows, and it
     * serves no purpose: none of them changes within a frame.
     */
    diag_skip_ps_text_ = REXCVAR_GET(bo2zm_native_diag_skip_ps);
    diag_vertices_repeated_ = REXCVAR_GET(bo2zm_native_diag_repeated_vertices);
    dedupe_active_ = REXCVAR_GET(bo2zm_native_dedupe_vertices);
    // bo2zm_native_vertex_cache, with the benchmark A/B's factor 1 when bo2zm_native_vertex_cache_ab. The DEBUG
    // page's switches (bo2zm_debug_options.cpp) turn this and the shortcuts below off where the A/B arms do.
    {
      bool on = vcache_ready_ && !vcache_off_ && REXCVAR_GET(bo2zm_native_vertex_cache) && !bo2zm::debug::CachesOff();
      if (on && vcache_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      vcache_active_ = on;
      // bo2zm_native_vertex_cache_sum, with the benchmark A/B's factor 2 when bo2zm_native_vertex_cache_sum_ab.
      bool sum = REXCVAR_GET(bo2zm_native_vertex_cache_sum);
      if (vcache_sum_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        if (arm >= 0) {
          sum = (arm & 2) != 0;
        }
      }
      vcache_sum_ = sum;
      ReportVertexCache();
    }
    // bo2zm_native_texture_by_writes, with the benchmark A/B's factor 1 when bo2zm_native_texture_by_writes_ab.
    if (tex_by_writes_ < 0) {
      tex_by_writes_ab_ = REXCVAR_GET(bo2zm_native_texture_by_writes_ab);
      tex_by_writes_ = REXCVAR_GET(bo2zm_native_texture_by_writes) || tex_by_writes_ab_ ? 1 : 0;
      if (tex_by_writes_) {
        bo2zm::page_writes::Enable(true);  // from now on, including the maps loaded later
      }
      REXLOG_INFO("[native] C3: texture rechecks by write stamps (bo2zm_native_texture_by_writes) = {}{}",
                  tex_by_writes_ ? "YES" : "no", tex_by_writes_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = tex_by_writes_ > 0 && !bo2zm::debug::TextureShortcutsOff();
      if (on && tex_by_writes_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      tex_by_writes_active_ = on;
    }
    // bo2zm_native_texture_by_writes_young, with the benchmark A/B's factor 1 when its _ab is set.
    if (tex_young_ < 0) {
      tex_young_ab_ = REXCVAR_GET(bo2zm_native_texture_by_writes_young_ab);
      tex_young_ = REXCVAR_GET(bo2zm_native_texture_by_writes_young) || tex_young_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C3: young textures skipped by write stamps too (bo2zm_native_texture_by_writes_young) = {}{}",
                  tex_young_ ? "YES" : "no", tex_young_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = tex_young_ > 0 && !bo2zm::debug::TextureShortcutsOff();
      if (on && tex_young_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      tex_young_active_ = on;
    }
    // bo2zm_native_index_cache, with the benchmark A/B's factor 1 when bo2zm_native_index_cache_ab.
    if (icache_ < 0) {
      icache_ab_ = REXCVAR_GET(bo2zm_native_index_cache_ab);
      icache_ = REXCVAR_GET(bo2zm_native_index_cache) || icache_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C6 index cache by write stamps (bo2zm_native_index_cache) = {}{}", icache_ ? "YES" : "no",
                  icache_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = icache_ > 0 && vcache_ready_ && !vcache_off_ &&
                bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed) && !bo2zm::debug::CachesOff();
      if (on && icache_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      icache_active_ = on;
    }
    ReportIndexCache();
    // bo2zm_native_constants_used, with the benchmark A/B's factor 1 when its _ab is set.
    if (constants_used_ < 0) {
      constants_used_ab_ = REXCVAR_GET(bo2zm_native_constants_used_ab);
      constants_used_ = REXCVAR_GET(bo2zm_native_constants_used) || constants_used_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C6 only the constants each shader reads (bo2zm_native_constants_used) = {}{}",
                  constants_used_ ? "YES" : "no", constants_used_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = constants_used_ > 0 && !bo2zm::debug::ConstantsShortcutOff();
      if (on && constants_used_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      constants_used_active_ = on;
    }
    // bo2zm_native_stamp_regions, with the benchmark A/B's factor 1 when its _ab is set.
    {
      static const bool regions_ab = REXCVAR_GET(bo2zm_native_stamp_regions_ab);
      static const bool regions = REXCVAR_GET(bo2zm_native_stamp_regions) || regions_ab;
      bool on = regions;
      if (on && regions_ab) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      bo2zm::page_writes::g_use_regions.store(on, std::memory_order_relaxed);
    }
    // bo2zm_native_prefetch_cache_lines, with the benchmark A/B's factor 1 when its _ab is set.
    if (prefetch_ < 0) {
      prefetch_ab_ = REXCVAR_GET(bo2zm_native_prefetch_cache_lines_ab);
      prefetch_ = REXCVAR_GET(bo2zm_native_prefetch_cache_lines) || prefetch_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C6 cache lines prefetched early (bo2zm_native_prefetch_cache_lines) = {}{}",
                  prefetch_ ? "YES" : "no", prefetch_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = prefetch_ > 0;
      if (on && prefetch_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      prefetch_lines_ = on;
    }
    // bo2zm_native_texture_reuse, with the benchmark A/B's factor 1 when bo2zm_native_texture_reuse_ab.
    {
      bool on = TextureReuse();
      if (on && texreuse_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      texreuse_active_ = on;
    }
    dedupe_indices_on_ = REXCVAR_GET(bo2zm_native_dedupe_indices) && !dedupe_indices_off_;
    // bo2zm_native_input_front with the benchmark A/B's factor 1 and
    // bo2zm_native_deferred_batch with its factor 2, when their _ab is set.
    {
      static const bool front_ab = REXCVAR_GET(bo2zm_native_input_front_ab);
      static const bool front = REXCVAR_GET(bo2zm_native_input_front) || front_ab;
      static const bool batch_ab = REXCVAR_GET(bo2zm_native_deferred_batch_ab);
      static const bool batch = REXCVAR_GET(bo2zm_native_deferred_batch) || batch_ab;
      const int arm = front_ab || batch_ab ? bo2zm::benchmark::AbArm() : -1;
      inputs_front_on_ = front && !inputs_front_off_ && (!front_ab || arm < 0 || (arm & 1) != 0);
      deferred::PublishInBatches(batch && (!batch_ab || arm < 0 || (arm & 2) != 0));
    }
    // bo2zm_native_fewer_copies with the benchmark A/B's factor 1 and
    // bo2zm_native_shared_swap with its factor 2, when their _ab is set.
    {
      static const bool fewer_ab = REXCVAR_GET(bo2zm_native_fewer_copies_ab);
      static const bool fewer = REXCVAR_GET(bo2zm_native_fewer_copies) || fewer_ab;
      static const bool swap_ab = REXCVAR_GET(bo2zm_native_shared_swap_ab);
      static const bool swap = REXCVAR_GET(bo2zm_native_shared_swap) || swap_ab;
      const int arm = fewer_ab || swap_ab ? bo2zm::benchmark::AbArm() : -1;
      fewer_copies_ = fewer && (!fewer_ab || arm < 0 || (arm & 1) != 0);
      shared_swap_ = swap && (!swap_ab || arm < 0 || (arm & 2) != 0);
    }
    vertices_base_cero_ = REXCVAR_GET(bo2zm_native_zero_base_vertices) && !vertices_base_zero_off_;
    diag_stats_draw_ = REXCVAR_GET(bo2zm_native_per_draw_stats_s) > 0;
    area_util_ = REXCVAR_GET(bo2zm_native_useful_area_pass);
    // The small per-draw savings.
    framing_cache_ = REXCVAR_GET(bo2zm_native_framing_cache) && !framing_cache_off_;
    height_useful_memo_active_ = REXCVAR_GET(bo2zm_native_useful_height_memo) && !height_useful_memo_off_;
    key_pass_fast_ = REXCVAR_GET(bo2zm_native_fast_pass_key) && !key_pass_fast_off_;
    cvars_per_frame_ = REXCVAR_GET(bo2zm_native_cvars_per_frame);
    ps_only_alpha_frame_ = PsOnlyAlpha();
    no_ps_no_color_frame_ = NoPsNoColor();
    no_vegetation_ = REXCVAR_GET(bo2zm_shadows_without_vegetation);
    pipelines_direct_ = REXCVAR_GET(bo2zm_native_direct_pipelines) && !pipelines_direct_off_;
    key_canonical_ = REXCVAR_GET(bo2zm_native_canonical_key) && !key_canonical_off_;  // phase 0a
    {
      const bool new_value = REXCVAR_GET(bo2zm_native_pipeline_across_passes);  // phase 0b
      if (new_value != pipeline_between_passes_ || !pipeline_between_passes_annotated_) {
        pipeline_between_passes_ = new_value;
        pipeline_between_passes_annotated_ = true;
        REXLOG_INFO("[native] C6 pipeline when a pass begins: {} (frame {})",
                    new_value ? "the binding is KEPT (dynamic state)" : "bound again, as before",
                    frame_);
      }
    }
    // Dynamic state phases 1 and 2, once per command buffer: the whole buffer uses one mode, because a
    // pipeline with a fixed state invalidates the dynamic value of that state.
    {
      uint32_t mode = 0;
      if (eds12_available_ && !eds_off_ && REXCVAR_GET(bo2zm_native_dynamic_state)) {
        mode |= kEds12;
      }
      if (eds3_available_ && !eds_off_ && REXCVAR_GET(bo2zm_native_dynamic_state3)) {
        mode |= kEds3;
      }
      if (mode != eds_mode_ || !eds_mode_annotated_) {
        eds_mode_annotated_ = true;
        REXLOG_INFO("[native] C6 dynamic state: {} (frame {})", NameModeEds(mode), frame_);
      }
      eds_mode_ = mode;
    }
    scissor_test_ = TestScissor();
    no_blur_frame_ = REXCVAR_GET(bo2zm_native_no_blur);  // once per frame
    // The cheap PCF bit also changes the pipeline: once per frame.
    {
      bool new_value = REXCVAR_GET(bo2zm_native_cheap_pcf);
      const int32_t alternate_pcf = REXCVAR_GET(bo2zm_native_cheap_pcf_alternate_s);
      if (alternate_pcf > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / alternate_pcf) % 2 == 1;
      }
      if (new_value != pcf_cheap_) {
        pcf_cheap_ = new_value;
        REXLOG_INFO("[native] C2 shadow map sampling: {}",
                    new_value ? "a single texel (cheap PCF)" : "3x3 pattern at half a texel");
      }
    }
    // The filter between mip levels is part of the sampler key, so it is also decided once per frame. Off
    // (the normal case), this is reading one cvar and comparing a bool.
    {
      bool new_value = REXCVAR_GET(bo2zm_native_test_point_mip);
      const int32_t alternate_mip = REXCVAR_GET(bo2zm_native_test_point_mip_alternate_s);
      if (alternate_mip > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / alternate_mip) % 2 == 1;
      }
      if (new_value != mip_point_test_) {
        mip_point_test_ = new_value;
        REXLOG_INFO("[native] C4 filter between mip levels: {}",
                    new_value ? "POINT (test: trilinear -> bilinear, the TMU does half)"
                          : "linear (what the game asks for)");
      }
    }
    // bo2zm_native_diag_min_mip. Part of the sampler key; when it changes the per-texture sampler caches
    // are invalidated (generation_textures_, as when an image is retired) so it shows in the next frame.
    // Keep the diagnostic separate: UI materials ignore preset texture reduction, but remain available
    // for deliberate mip diagnostics. Changing either value invalidates cached sampler bindings.
    {
      const uint32_t new_value = uint32_t(std::clamp(int32_t(REXCVAR_GET(bo2zm_native_diag_min_mip)), 0, 4));
      const uint32_t new_preset = std::min(4u, bo2zm::preset::MipsMinus());
      if (new_value != diag_mip_min_ || new_preset != preset_mip_min_) {
        diag_mip_min_ = new_value;
        preset_mip_min_ = new_preset;
        ++generation_textures_;
        REXLOG_INFO("[native] C4 texture mip floor: world {}, UI {} (preset {}, diagnostic {})",
                    std::max(new_value, new_preset), new_value, new_preset, new_value);
      }
    }
    // The shadow map's own depth bias, once per frame.
    {
      const int32_t constant = std::clamp(int32_t(REXCVAR_GET(bo2zm_native_shadow_bias_constant)), 0, 100);
      const int32_t pending = std::clamp(int32_t(REXCVAR_GET(bo2zm_native_shadow_bias_slope)), 0, 100);
      if (constant != shadows_bias_constant_ || pending != shadows_bias_pending_ || !shadows_bias_annotated_) {
        shadows_bias_constant_ = constant;
        shadows_bias_pending_ = pending;
        shadows_bias_annotated_ = true;
        REXLOG_INFO("[native] C4 shadow map depth bias: slope {:.1f}, constant {} (x1000 24-bit units){}",
                    double(pending) * 0.1, constant, (constant || pending) ? "" : " (off, as before)");
      }
    }
    // The 1/size bit changes the pipeline, so it is decided once per frame.
    {
      const int32_t alternate = REXCVAR_GET(bo2zm_native_inv_tex_size_alternate_s);
      bool new_value = REXCVAR_GET(bo2zm_native_inv_tex_size);
      if (alternate > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / alternate) % 2 == 1;
      }
      if (new_value != inv_size_tex_) {
        inv_size_tex_ = new_value;
        REXLOG_INFO("[native] C2 texture size: {}",
                    new_value ? "by constant" : "asked from the texture");
      }
    }
    /*
     * The early depth test also changes the pipeline (a different module), so it is decided once per
     * frame, like the PCF and the 1/size.
     */
    {
      const int32_t alternate = REXCVAR_GET(bo2zm_native_early_z_alternate_s);
      bool new_value = REXCVAR_GET(bo2zm_native_early_z);
      if (alternate > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_z_)
                                  .count();
        new_value = (seconds / alternate) % 2 == 1;
      }
      if (new_value != z_early_ || !alternation_z_annotated_) {
        z_early_ = new_value;
        alternation_z_annotated_ = true;
        REXLOG_INFO("[native] C6 depth test: {} (frame {})",
                    new_value ? "BEFORE shading where Z is not written (early Z)"
                          : "after shading, as before",
                    frame_);
      }
    }
    skip_invisible_ = REXCVAR_GET(bo2zm_native_skip_invisible);
    // The deferred sky, also once per frame. If it is switched off mid-frame with one already deferred, the
    // pending one is still emitted through its usual path.
    {
      const bool new_value = REXCVAR_GET(bo2zm_native_postponed_sky);
      if (new_value != sky_postpone_on_ || !sky_annotated_) {
        sky_postpone_on_ = new_value;
        sky_annotated_ = true;
        REXLOG_INFO("[native] C6 sky: {} (frame {})",
                    new_value ? "POSTPONED until after the pass's opaque draws"
                          : "in its original place (first, on the empty Z)",
                    frame_);
      }
    }
    ReportZEarly();
    ReportPipelinesDirect();
    ReportMinor();
    ReportBaseZero();
    ControlSet4();  // set 4 by differences in NVK (cvar, alternation and guard)
    ReportSet4();
    ControlDrawNvk();  // the draw path in NVK (measurement, improvements and guards)
    ReportDrawNvk();
    ControlCopiesNvk();  // state copy by fingerprints and copies on the copy engine
    ReportCopiesNvk();
    ControlKindViewsNvk();  // pool textures share one mapping per block
    ReportKeyCanonical();  // Dynamic state, phase 0a
    ReportPipelineBetweenPasses();  // Dynamic state, phase 0b
    ReportStateDynamic();  // dynamic state phases 1 and 2
    ReportBindings();  // texture bind thread, every 10 s
    ReportVolumes();  // 3D textures read from guest memory, every 10 s
    ReportHashes();  // texture hash thread, every 10 s
    ReportReuse();  // measurement only: new textures with a live one's content, every 10 s
    ReportTextureStamps();  // texture rechecks by write stamps, every 10 s
    ReportTextureReuse();  // new textures that took a cold one's image, every 10 s
    EvictTexturesYesDoesMissing();
    pool_textures_.PerFrame(frame_);  // grows with headroom, never exactly on demand
    // Test bo2zm_native_constants_ubo_alternate_s. Switched here because the new epoch forces all constants
    // to be allocated again: nothing allocated in one mode is reused in the other.
    if (alternate_ubo_s_ > 0) {
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_ubo_).count();
      const bool ubo = (seconds / alternate_ubo_s_) % 2 == 1;
      if (ubo != use_ubo_) {
        use_ubo_ = ubo;
        // With UBOs no push constants are recorded, so when going back to pointers the tracked state is
        // stale.
        std::memset(push_recorded_, 0, sizeof(push_recorded_));
        REXLOG_INFO("[native] constants test: {} (frame {})", ubo ? "by UBO" : "by pointer", frame_);
      }
    }
    ++submits_;  // "C6 constants by UBO" report
    if (use_ubo_) {
      ++submits_ubo_;
    }
    // What the prewarm thread actually compiles also goes into the cache and has to be saved.
    if (const uint32_t compiled = prewarm_compiled_.load(std::memory_order_relaxed) +
                                  prewarm_compiled_helper_.load(std::memory_order_relaxed);
        compiled != prewarm_counted_) {
      pipelines_no_save_ += compiled - prewarm_counted_;
      prewarm_counted_ = compiled;
    }
    // The pipeline cache is saved after 64 new pipelines, or after a minute with any new one: on the
    // Switch, exiting does not always reach the destructor. In a match, as
    // bo2zm_native_pipelines_save_in_match says.
    SaveCacheWhenDue();
  }

  // End of a frame, ring thread.
  void SaveCacheWhenDue() {
    const auto now = std::chrono::steady_clock::now();
    if (pipelines_no_save_ != pipelines_seen_) {
      pipelines_seen_ = pipelines_no_save_;
      pipeline_new_at_ = now;
    }
    const bool in_match = bo2zm::cheats::InMatch();
    const int mode = REXCVAR_GET(bo2zm_native_pipelines_save_in_match);
    const bool due =
        pipelines_no_save_ && (pipelines_no_save_ >= 64 || now - cache_saved_ >= std::chrono::seconds(60));
    if (!in_match || mode == 2) {
      if (due || (!in_match && writer_list_waiting_.load(std::memory_order_relaxed))) {
        SaveCachePipelines(false);
      }
    } else if (mode == 1 && pipelines_no_save_ && now - pipeline_new_at_ >= std::chrono::seconds(30)) {
      SaveCachePipelines(true);
    }
    // The prewarm walked the whole list without compiling anything (the precompile off or not run): the
    // file on disk then holds every listed pipeline, and the stamp file says so for the next start.
    // Both prewarm threads ended and all they compiled already saved (counted into pipelines_no_save_, then written).
    if (!vouch_requested_ && !pipelines_no_save_ && !list_no_save_ && !bo2zm::native::precompile::KnownWarm() &&
        prewarm_finished_.load(std::memory_order_acquire) &&
        (!prewarm_helper_.joinable() || prewarm_helper_done_.load(std::memory_order_acquire)) &&
        prewarm_compiled_.load(std::memory_order_relaxed) + prewarm_compiled_helper_.load(std::memory_order_relaxed) ==
            prewarm_counted_ &&
        ListVerified()) {
      RequestVouch();
    }
    // bo2zm_native_test_export_list
    if (REXCVAR_GET(bo2zm_native_test_export_list) && list_session_.size() != exported_size_ &&
        now - exported_at_ >= std::chrono::seconds(20)) {
      exported_size_ = list_session_.size();
      exported_at_ = now;
      std::vector<uint8_t> list = SerializeListPipelines();
      std::lock_guard<std::mutex> lock(writer_mutex_);
      StartWriterCache();
      writer_export_ = std::move(list);
      writer_pending_ = true;
      writer_warning_.notify_one();
    }
    // bo2zm_native_test_pipelines_resave_s
    if (const int every = REXCVAR_GET(bo2zm_native_test_pipelines_resave_s); every > 0 && in_match) {
      if (resave_at_ == std::chrono::steady_clock::time_point{}) {
        resave_at_ = now + std::chrono::seconds(every);
      } else if (now >= resave_at_) {
        resave_at_ = now + std::chrono::seconds(every);
        {
          std::lock_guard<std::mutex> lock(writer_mutex_);
          writer_resave_ = (resaves_++ % 2) ? 2 : 1;
        }
        SaveCachePipelines(false);
      }
    }
  }

  void InvalidateTextures() override {
    ++generation_textures_;
    ++invalidations_global_;
  }

  // BO2 swaps resolve images about four times per frame (GetResolved's parked ones). Dropping the whole
  // sampler cache on each swap made 227,000 of the PC's 260,000 table misses in 10 s of Town "by generation", and on
  // the Switch the sampler loop then took 6.5 us of a ~10 us draw. So only the entries for that address are dropped,
  // plus every cubemap (PrepareTexture looks its faces up at base + face * stride).
  // The table is walked through cache_fetch_dir_ (16 KB) rather than the entries themselves (256 KB): a full walk
  // of the entries costs ~95 us per copy on the Switch.
  void InvalidateTexturesIn(uint32_t address) override {
    address &= 0x1FFFF000u;
    for (CacheSampler& c : cache_samplers_) {
      const uint32_t d = AddressCacheSampler(c.fetch.data());
      if ((d & ~1u) == address || (d & 1u)) {
        c.frame = UINT64_MAX;
        c.valid_until = 0;
      }
    }
    // Eight entries per step, branch-free (the compiler vectorizes it), and only the rare blocks with a
    // match go back over their entries. ~1,200 calls per second over 4,096 entries were ~11 ms/s of the PC's ring.
    static_assert(std::tuple_size_v<decltype(cache_fetch_dir_)> % 8 == 0, "the table is walked eight at a time");
    for (size_t i = 0; i < cache_fetch_dir_.size(); i += 8) {
      uint32_t hit = 0;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t d = cache_fetch_dir_[i + k];
        hit |= uint32_t(((d & ~1u) == address) | (d & 1u)) << k;
      }
      while (hit) [[unlikely]] {
        const uint32_t k = uint32_t(std::countr_zero(hit));
        hit &= hit - 1;
        cache_fetch_[i + k].frame = UINT64_MAX;
        cache_fetch_[i + k].valid_until = 0;
      }
    }
    ++invalidations_targeted_;
  }

  // A sampler cache entry's texture address, with bit 0 set for a cubemap (its faces are other addresses).
  static uint32_t AddressCacheSampler(const uint32_t* fetch) {
    return (fetch[1] & 0x1FFFF000u) |
           (((fetch[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::kCube) ? 1u : 0u);
  }

  // Only the cache entries that use a view of those images. The views stay valid (they go with their
  // image); what has to be redone is which slot each fetch constant gets. By their slots' generations,
  // without walking the caches.
  void InvalidateImages(gpu::Image a, gpu::Image b) override {
    for (VkImage image : {a, b}) {
      if (image == VK_NULL_HANDLE || (image == b && a == b)) {
        continue;
      }
      const auto it = views_per_image_.find(image);
      if (it == views_per_image_.end()) {
        continue;
      }
      for (uint64_t key : it->second) {
        const auto v = views_.find(key);
        if (v != views_.end() && v->second.image == image) {
          RetireSlot(v->second.heap, v->second.slot);
        }
      }
    }
  }

  void OcclusionOpen(bool open) override {
    if (!open && query_occlusion_ != UINT32_MAX) {
      // Still inside the pass of the last counted draw: FinishPass would have closed it already.
      context_->FinishQueryOcclusion(query_occlusion_);
      query_occlusion_ = UINT32_MAX;
    }
    occlusion_open_ = open;
  }

  uint64_t Drawn() const override { return drawn_; }

  // bo2zm_native_textures_mb_max: with the texture cache above the limit, evict the textures unused for
  // the longest until it drops to 75 %, at most once every 60 frames.
  // It is safe without waiting for the GPU: a texture not prepared for 120 frames cannot be referenced by
  // any pending submission. The slot caches are valid for at most 32 frames after preparing, and only the
  // previous submission can still be on the GPU (Record waits for the slot before reusing it). The heap
  // slots are updated with UPDATE_AFTER_BIND, and the new generation invalidates the caches.
  static constexpr uint64_t kFramesNoUseForRelease = 120;
  /*
   * How much GPU memory there is and how much is left.
   *
   * The cache limit was picked by eye twice (384 MB and then 192) because this data was not available.
   * With VK_EXT_memory_budget the driver reports the budget and the usage; without it, at least the heap
   * sizes are visible. It goes with the cache report, every 256 new textures.
   */
  void AnnotateGpuMemory() {
    const auto* instance = vulkan_device_->vulkan_instance();
    if (!instance) {
      return;
    }
    const auto& ifn = instance->functions();
    const VkPhysicalDevice physical = vulkan_device_->physical_device();
    const bool has_budget = vulkan_device_->extensions().ext_EXT_memory_budget &&
                                 ifn.vkGetPhysicalDeviceMemoryProperties2 != nullptr;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties2.pNext = has_budget ? &budget : nullptr;
    if (has_budget) {
      ifn.vkGetPhysicalDeviceMemoryProperties2(physical, &properties2);
    } else {
      ifn.vkGetPhysicalDeviceMemoryProperties(physical, &properties2.memoryProperties);
    }
    const auto& mp = properties2.memoryProperties;
    std::string text;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
      if (!text.empty()) {
        text += " | ";
      }
      const bool local = (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
      if (has_budget) {
        text += fmt::format("heap {}{}: {} MB used of {} MB budgeted (size {} MB)", i,
                             local ? " (GPU)" : "", budget.heapUsage[i] >> 20,
                             budget.heapBudget[i] >> 20, mp.memoryHeaps[i].size >> 20);
      } else {
        text += fmt::format("heap {}{}: {} MB", i, local ? " (GPU)" : "",
                             mp.memoryHeaps[i].size >> 20);
      }
    }
    REXLOG_INFO("[native] C3 GPU memory: {}{}; the texture cache holds {} MB of {} MB; texture slots in use: 2D {} of "
                "{}, 3D {} of {}, cube {} of {}",
                text, has_budget ? "" : " (without VK_EXT_memory_budget: sizes only)", bytes_textures_ >> 20,
                textures_mb_max_, SlotsUsed(0), kCapacityHeap[0], SlotsUsed(1), kCapacityHeap[1], SlotsUsed(2),
                kCapacityHeap[2]);
    // Without this line there is no way to know whether the pool is working or fragmenting. A vegetation
    // counter once existed and was never printed.
    if (pool_textures_.IsActive()) {
      REXLOG_INFO("[native] C3 {}", pool_textures_.Summary());
    }
  }

  /*
   * Without stutters.
   *
   * Waiting until the limit is exceeded and then evicting 25 % at once is a stutter in gameplay, and a
   * stutter every few minutes is no better than running out of memory every fourteen.
   *
   * Instead, a few are evicted per frame as soon as the cache nears the limit, and only cold textures: those
   * unused for 120 frames, i.e. two to four seconds. They are not in the working set, so they do not have
   * to be uploaded again right away and there is no thrashing. If there are not enough cold ones, nothing
   * is forced: the cache is allowed to grow, since going a little over is better than uploading and
   * evicting the same thing.
   */
  static constexpr uint32_t kReleasePerFrame = 4;

  // Whether a texture heap (2D, 3D or cube) has 80 % of its slots in use. The cache's limit is in
  // megabytes, and with small textures it held more of them than the 2D heap had slots (kCapacityHeap).
  bool SlotsTight() const {
    for (uint32_t i = 0; i < 3; ++i) {
      const auto& h = heaps_[i];
      const uint64_t used = h.next - 1 - std::min<uint64_t>(h.free.size(), h.next - 1);
      if (h.capacity && used * 5 >= uint64_t(h.capacity) * 4) {
        return true;
      }
    }
    return false;
  }

  void EvictTexturesYesDoesMissing() {
    const uint64_t limit = uint64_t(std::max(textures_mb_max_, 0)) << 20;
    // Little by little, from 75 % of the limit or 80 % of a heap's slots. That is what avoids the stutter.
    if ((limit && bytes_textures_ > limit / 4 * 3) || SlotsTight()) {
      ReleaseSomeFewCold();
    }
    if (!limit) {
      return;
    }
    // Safety net: if it still goes over the limit (because almost everything is hot), one batch every 60
    // frames. With the above working, this should almost never trigger.
    if (bytes_textures_ > limit && frame_ >= attempt_eviction_ + 60) {
      attempt_eviction_ = frame_;
      ReleaseTextures(limit / 4 * 3, kFramesNoUseForRelease, "above the limit");
    }
  }

  // Whether a texture may be released by the gradual eviction: cold, with an image, and nothing pending on it.
  bool ColdForRelease(const CachedTexture& texture) const {
    return texture.image.image != VK_NULL_HANDLE && !texture.needs_upload && !texture.in_flight && !texture.hash_job &&
           texture.frame != UINT64_MAX && texture.frame + kFramesNoUseForRelease < frame_;
  }

  bool EvictByKey() {
    static const bool on = REXCVAR_GET(bo2zm_native_evict_by_key);
    static const bool ab = REXCVAR_GET(bo2zm_native_evict_by_key_ab);
    if (ab) {
      const int arm = bo2zm::benchmark::AbArm();
      if (arm >= 0) {
        return (arm & 1) != 0;
      }
    }
    return on;
  }

  // bo2zm_native_evict_by_key: the kReleasePerFrame oldest cold ones from a list of the kColdKeep oldest, gathered
  // every kColdScanEvery frames (or sooner when it ran out while more were cold). Each is checked again when
  // taken: one used since then is skipped.
  static constexpr uint64_t kColdScanEvery = 16;
  static constexpr size_t kColdKeep = 64;

  void ReleaseSomeFewColdByKey() {
    const bool due = frame_ >= cold_scan_frame_ + kColdScanEvery || (cold_list_.empty() && cold_scan_full_);
    if (due) {
      cold_list_.clear();
      for (const auto& [key, texture] : textures_) {
        if (ColdForRelease(texture)) {
          cold_list_.emplace_back(texture.frame, key);
        }
      }
      cold_scan_full_ = cold_list_.size() > kColdKeep;
      if (cold_scan_full_) {
        std::nth_element(cold_list_.begin(), cold_list_.begin() + kColdKeep, cold_list_.end());
        cold_list_.resize(kColdKeep);
      }
      // Newest first, so pop_back takes the oldest.
      std::sort(cold_list_.begin(), cold_list_.end(), std::greater<>());
      cold_scan_frame_ = frame_;
      ++cold_scans_;
    }
    keys_release_.clear();
    while (keys_release_.size() < kReleasePerFrame && !cold_list_.empty()) {
      const uint64_t key = cold_list_.back().second;
      cold_list_.pop_back();
      const auto it = textures_.find(key);
      if (it == textures_.end() || !ColdForRelease(it->second)) {
        continue;
      }
      keys_release_.push_back(key);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    if (keys_release_.empty()) {
      return;  // all hot: let it grow rather than thrash
    }
    ReleaseKeys(keys_release_);
    textures_released_ += keys_release_.size();
    released_little_a_little_ += keys_release_.size();
    if (frame_ >= warning_trickle_ + 600) {  // one warning every 600 frames, not one per texture
      warning_trickle_ = frame_;
      REXLOG_INFO("[native] C3: texture cache near the limit: cold ones are released a trickle at a time ({} in total, "
                  "by key, {} scans); {} textures and {} MB of {} MB remain",
                  released_little_a_little_, cold_scans_, textures_.size(), bytes_textures_ >> 20, textures_mb_max_);
    }
  }

  // ReleaseImages for these textures only: their views through views_per_image_ (as ForgetImage), then the images.
  void ReleaseKeys(const std::vector<uint64_t>& keys) {
    ++invalidations_retired_;
    for (const uint64_t key : keys) {
      const auto it = textures_.find(key);
      if (it == textures_.end()) {
        continue;
      }
      const VkImage image = it->second.image.image;
      const auto index = image != VK_NULL_HANDLE ? views_per_image_.find(image) : views_per_image_.end();
      if (index != views_per_image_.end()) {
        for (const uint64_t view_key : index->second) {
          const auto v = views_.find(view_key);
          if (v == views_.end() || v->second.image != image) {
            continue;
          }
          RetireSlot(v->second.heap, v->second.slot);  // only the cache entries that point at it
          WriteImage(v->second.heap, v->second.slot, empty_[v->second.heap].view);
          heaps_[v->second.heap].free.push_back(v->second.slot);
          dfn_.vkDestroyImageView(device_, v->second.view, nullptr);
          views_.erase(v);
        }
        views_per_image_.erase(index);
      }
      RemoveContentTexture(it->second, it->first);  // measurement only: bo2zm_native_diag_reuse
      DestroyImage(it->second.image);
      textures_.erase(it);
    }
  }

  // The kReleasePerFrame oldest cold ones. Without sorting the whole cache: they are picked on the fly.
  void ReleaseSomeFewCold() {
    if (EvictByKey()) {
      ReleaseSomeFewColdByKey();
      return;
    }
    std::array<std::pair<uint64_t, uint64_t>, kReleasePerFrame> chosen{};  // (frame, key)
    uint32_t how_many = 0;
    for (const auto& [key, texture] : textures_) {
      // Nor those with a bind in flight, nor those owned by the hash thread.
      if (texture.image.image == VK_NULL_HANDLE || texture.needs_upload || texture.in_flight || texture.hash_job ||
          texture.frame == UINT64_MAX ||
          texture.frame + kFramesNoUseForRelease >= frame_) {
        continue;
      }
      if (how_many < kReleasePerFrame) {
        chosen[how_many++] = {texture.frame, key};
        continue;
      }
      // Replaces the most recent of the chosen ones, if this one is older.
      uint32_t worst = 0;
      for (uint32_t i = 1; i < how_many; ++i) {
        if (chosen[i].first > chosen[worst].first) {
          worst = i;
        }
      }
      if (texture.frame < chosen[worst].first) {
        chosen[worst] = {texture.frame, key};
      }
    }
    if (how_many == 0) {
      return;  // all hot: let it grow rather than thrash
    }
    std::unordered_set<VkImage> images;
    for (uint32_t i = 0; i < how_many; ++i) {
      auto it = textures_.find(chosen[i].second);
      if (it == textures_.end()) {
        continue;
      }
      images.insert(it->second.image.image);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    ReleaseImages(images);
    textures_released_ += images.size();
    released_little_a_little_ += images.size();
    if (frame_ >= warning_trickle_ + 600) {  // one warning every 600 frames, not one per texture
      warning_trickle_ = frame_;
      REXLOG_INFO("[native] C3: texture cache near the limit: cold ones are released a trickle at a time ({} in total); "
                  "{} textures and {} MB of {} MB remain",
                  released_little_a_little_, textures_.size(), bytes_textures_ >> 20, textures_mb_max_);
    }
  }

  /*
   * The last resort, when the GPU has no more memory to give.
   *
   * Once nvMapCreate started failing even for 64 KB there was no way back: the driver ended up
   * quarantining ranges and the screen went black with the audio still playing. Before it gets to that,
   * half the texture cache is released.
   *
   * Normal eviction only touches what has been unused for 120 frames, because images are destroyed at
   * once and the GPU could still be reading them. Here that margin is not needed: it waits for the GPU to
   * finish everything in flight, and then none is in use. It costs a one-frame stutter; crashing costs
   * the game.
   */
  bool ReleaseTexturesOnOutOfMemory() override {
    return ReleaseTextureMemory(nullptr);
  }

  bool ReleaseTextureMemory(const NativeImage* keep) {
    if (textures_.empty() || bytes_textures_ == 0 || releasing_on_out_of_memory_) {
      return false;  // the guard keeps an allocation inside the cleanup itself from re-entering here
    }
    releasing_on_out_of_memory_ = true;
    bool algo = false;
    if (context_ && context_->WaitGpuIdle()) {
      algo = ReleaseTextures(bytes_textures_ / 2, 0, "OUT OF GPU MEMORY", keep) > 0;
    }
    releasing_on_out_of_memory_ = false;
    return algo;
  }

  // Actually retires a group of images. First the views and their slots (pointed at the empty texture),
  // then the images. Used by both the gradual and the batch eviction.
  void ReleaseImages(const std::unordered_set<VkImage>& images) {
    ++invalidations_retired_;
    for (auto it = views_.begin(); it != views_.end();) {
      if (!images.count(it->second.image)) {
        ++it;
        continue;
      }
      RetireSlot(it->second.heap, it->second.slot);  // only the cache entries that point at it
      WriteImage(it->second.heap, it->second.slot, empty_[it->second.heap].view);
      heaps_[it->second.heap].free.push_back(it->second.slot);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_per_image_.erase(it->second.image);  // all views of that image go
      it = views_.erase(it);
    }
    for (auto it = textures_.begin(); it != textures_.end();) {
      if (!images.count(it->second.image.image)) {
        ++it;
        continue;
      }
      RemoveContentTexture(it->second, it->first);  // measurement only: bo2zm_native_diag_reuse
      DestroyImage(it->second.image);
      it = textures_.erase(it);
    }
  }

  // Evicts unused textures until below `goal`. Returns the bytes freed.
  uint64_t ReleaseTextures(uint64_t goal, uint64_t min_age, const char* reason,
                           const NativeImage* keep = nullptr) {
    std::vector<std::pair<uint64_t, uint64_t>> candidates;  // (last frame it was prepared, key)
    for (const auto& [key, texture] : textures_) {
      // Nor those with a bind in flight (their image has no memory yet), nor those owned by the hash thread.
      if (&texture.image != keep && texture.image.image != VK_NULL_HANDLE &&
          !texture.needs_upload && !texture.in_flight && !texture.hash_job &&
          texture.frame != UINT64_MAX &&
          texture.frame + min_age < frame_) {
        candidates.emplace_back(texture.frame, key);
      }
    }
    std::sort(candidates.begin(), candidates.end());
    const uint64_t before = bytes_textures_;
    std::unordered_set<VkImage> images;
    for (const auto& [last, key] : candidates) {
      if (bytes_textures_ <= goal) {
        break;
      }
      auto it = textures_.find(key);
      images.insert(it->second.image.image);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    if (images.empty()) {
      return 0;
    }
    ReleaseImages(images);
    textures_released_ += images.size();
    REXLOG_INFO("[native] C3: texture cache {} ({} MB): {} unused for more than {} frames removed ({} MB); {} textures "
                "and {} MB remain ({} released in total)",
                reason, before >> 20, images.size(), min_age,
                (before - bytes_textures_) >> 20, textures_.size(), bytes_textures_ >> 20, textures_released_);
    return before - bytes_textures_;
  }

  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR. Needed for images created without
  // TRANSFER_DST (the only ones the driver gives a ZCULL plane), because vkCmdClearDepthStencilImage
  // requires that usage (VUID-vkCmdClearDepthStencilImage-pRanges-02660). It costs no extra GPU time: it
  // is exactly how NVK implements that command internally (nvk_cmd_clear.c, clear_image opens a pass with
  // loadOp = CLEAR).
  bool ClearDepthInPass(gpu::CommandList commands, const NativeImage& image,
                               float depth, uint32_t stencil) override {
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !image.width ||
        !image.height) {
      return false;
    }
    FinishPass();  // a pass cannot be opened inside another
    uint32_t formats[5] = {0, 0, 0, 0, uint32_t(image.format)};
    const VkRenderPass pass = PassOf(formats, kLoadClear);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[4] = image.view;
    const VkFramebuffer framebuffer = FramebufferOf(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear{};
    clear.depthStencil = {depth, stencil};
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea.extent = {image.width, image.height};
    start.clearValueCount = 1;
    start.pClearValues = &clear;
    PassBarrier(commands);
    gpu_.BeginPass(commands, start);
    gpu_.EndPass(commands);
    PassBarrier(commands);
    ++clears_depth_in_pass_;
    return true;
  }

  // bo2zm_native_clear_useful_area. Clears only the `area` rectangle of a color image with a
  // loadOp = CLEAR pass: what NVK does internally for vkCmdClearColorImage (nvk_cmd_clear.c), over that
  // rectangle. What lies outside is not touched (Vulkan only loads and stores the renderArea).
  bool ClearColorInPass(gpu::CommandList commands, const NativeImage& image, const gpu::ClearColor& clear_color,
                        const gpu::Rect& clear_area) override {
    const VkClearColorValue color = gpu::ToVk(clear_color);
    const VkRect2D area = gpu::ToVk(clear_area);
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !area.extent.width || !area.extent.height ||
        area.offset.x < 0 || area.offset.y < 0 || uint32_t(area.offset.x) + area.extent.width > image.width ||
        uint32_t(area.offset.y) + area.extent.height > image.height) {
      return false;
    }
    FinishPass();  // a pass cannot be opened inside another
    uint32_t formats[5] = {uint32_t(image.format), 0, 0, 0, 0};
    const VkRenderPass pass = PassOf(formats, kLoadClear);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[0] = image.view;
    const VkFramebuffer framebuffer = FramebufferOf(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear{};
    clear.color = color;
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    start.clearValueCount = 1;
    start.pClearValues = &clear;
    PassBarrier(commands);
    gpu_.BeginPass(commands, start);
    gpu_.EndPass(commands);
    PassBarrier(commands);
    return true;
  }

  // BO2's Direct3D Clear() is a draw (a rectangle with D3D's own shaders), so it can clear part of a
  // render target, or only its stencil. The pass loads the image (loadOp = LOAD) and vkCmdClearAttachments
  // writes only the rectangle and the aspects requested.
  bool ClearAttachmentInPass(gpu::CommandList commands, const NativeImage& image, gpu::Aspects clear_aspects,
                             const gpu::ClearValue& clear_value, const gpu::Rect& clear_area) override {
    const VkImageAspectFlags aspects = gpu::ToVk(clear_aspects);
    const VkClearValue value = gpu::ToVk(clear_value);
    const VkRect2D area = gpu::ToVk(clear_area);
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !aspects || !area.extent.width ||
        !area.extent.height || area.offset.x < 0 || area.offset.y < 0 ||
        uint32_t(area.offset.x) + area.extent.width > image.width ||
        uint32_t(area.offset.y) + area.extent.height > image.height) {
      return false;
    }
    FinishPass();  // a pass cannot be opened inside another
    const uint32_t slot = (aspects & VK_IMAGE_ASPECT_COLOR_BIT) ? 0 : 4;
    uint32_t formats[5] = {};
    formats[slot] = uint32_t(image.format);
    const VkRenderPass pass = PassOf(formats, kLoadRead);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[slot] = image.view;
    const VkFramebuffer framebuffer = FramebufferOf(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    PassBarrier(commands);
    gpu_.BeginPass(commands, start);
    VkClearAttachment attachment{};
    attachment.aspectMask = aspects;
    attachment.colorAttachment = 0;
    attachment.clearValue = value;
    VkClearRect rectangle{};
    rectangle.rect = area;
    rectangle.baseArrayLayer = 0;
    rectangle.layerCount = 1;
    gpu_.ClearAttachments(commands, 1, &attachment, 1, &rectangle);
    gpu_.EndPass(commands);
    PassBarrier(commands);
    return true;
  }

  // bo2zm_native_framebuffers_forget_views. C2 is about to destroy this view (Destroy, with the GPU
  // idle for that image): the FramebufferOf framebuffers that use it are destroyed and removed from the
  // cache. It walks the created framebuffers (dozens); never during a draw.
  // Self-checking guard: the cache and its view list must stay in step (only FramebufferOf inserts) and
  // no pass may be open (whoever destroys the view has already submitted the work). Otherwise: MISMATCH
  // in the log and, for the rest of the run, nothing is destroyed at runtime: the affected framebuffers
  // (or the whole cache, if they are out of step) are retired and destroyed at shutdown, which is never
  // worse than keeping everything until the destructor.
  void ForgetView(gpu::ImageView view) override {
    if (view == VK_NULL_HANDLE || !REXCVAR_GET(bo2zm_native_framebuffers_forget_views)) {
      return;
    }
    const bool a_pair = framebuffers_views_.size() == framebuffers_.size();
    if (!fb_retire_ && (pass_active_ || !a_pair)) {
      fb_retire_ = true;
      REXLOG_ERROR("[native] C6 framebuffers: MISMATCH when destroying view {:016X}: {}. For the rest of "
                   "the session the affected framebuffers are retired without destroying them (they are destroyed on "
                   "close)",
                   uint64_t(reinterpret_cast<uintptr_t>(view)),
                   pass_active_ ? "a pass is open (whoever destroys the view has not submitted the work)"
                                : "the cache and its list of views are out of step (someone creates framebuffers "
                                  "another way)");
    }
    uint32_t outside = 0;
    if (!a_pair) {
      // Unknown which ones use the view: drop them all (they are rebuilt on request).
      for (auto& [key, framebuffer] : framebuffers_) {
        fb_retired_.push_back(framebuffer);
      }
      outside = uint32_t(framebuffers_.size());
      framebuffers_.clear();
      framebuffers_views_.clear();
    } else {
      for (auto it = framebuffers_views_.begin(); it != framebuffers_views_.end();) {
        if (std::find(it->second.begin(), it->second.end(), view) == it->second.end()) {
          ++it;
          continue;
        }
        const auto fb = framebuffers_.find(it->first);
        if (fb != framebuffers_.end()) {
          if (fb_retire_) {
            fb_retired_.push_back(fb->second);
          } else {
            dfn_.vkDestroyFramebuffer(device_, fb->second, nullptr);
          }
          framebuffers_.erase(fb);
          ++outside;
        }
        it = framebuffers_views_.erase(it);
      }
    }
    ++fb_views_forgotten_;
    fb_forgotten_ += outside;
    if (fb_views_forgotten_ <= 16 || (fb_views_forgotten_ & 63) == 0) {
      BO2ZM_REPORT_RING("[native] C6 framebuffers: view {:016X} destroyed: {} framebuffers {} with it; {} "
                           "remain in the cache ({} views and {} framebuffers since boot; {} retired)",
                           uint64_t(reinterpret_cast<uintptr_t>(view)), outside,
                           fb_retire_ ? "retired (not destroyed)" : "destroyed", framebuffers_.size(),
                           fb_views_forgotten_, fb_forgotten_, fb_retired_.size());
    }
  }

  void ForgetImage(gpu::Image image) override {
    ++invalidations_retired_;
    const auto index = views_per_image_.find(image);  // without walking views_
    if (index == views_per_image_.end()) {
      return;
    }
    for (uint64_t key : index->second) {
      const auto it = views_.find(key);
      if (it == views_.end() || it->second.image != image) {
        continue;
      }
      RetireSlot(it->second.heap, it->second.slot);  // the sampler cache entries that point at it
      WriteImage(it->second.heap, it->second.slot, empty_[it->second.heap].view);
      heaps_[it->second.heap].free.push_back(it->second.slot);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_.erase(it);
    }
    views_per_image_.erase(index);
  }

  DrawStats Stats() const override {
    DrawStats e;
    e.drawn = drawn_;
    e.rejected = rejected_;
    e.pipelines = pipelines_.size();
    e.textures = textures_.size();
    e.uploads_texture = uploads_texture_;
    e.megs_uploaded = megs_bytes_ >> 20;
    e.megs_textures = bytes_textures_ >> 20;
    e.ms_pipelines = ns_pipelines_ / 1000000;
    e.passes = passes_begun_;
    e.submits_full = submits_full_;
    e.ns_submits_full = ns_submits_full_;
    e.bytes_vertices = bytes_vertices_;
    e.dedupe_hits = dedupe_.hits();
    e.dedupe_bytes = dedupe_.bytes_saved();
    e.dedupe_collisions = dedupe_.collisions();
    e.bytes_indices = bytes_indices_uploaded_;
    e.samplers = samplers_prepared_;
    e.samplers_cache = samplers_cache_ + samplers_cache_fetch_;
    e.ns_passes = ns_passes_;
    e.ns_vertices = ns_vertices_;
    e.inputs_computed = inputs_computed_;
    e.ns_inputs = ns_inputs_;
    e.inputs_reused = inputs_reused_ + inputs_cache_hits_;
    e.passes_per_generation = passes_per_generation_;
    e.passes_per_target = passes_per_target_;
    e.passes_resumed = passes_resumed_;
    e.ns_render_pass = ns_render_pass_;
    e.texels_passes = texels_passes_;
    e.texels_per_category = texels_per_category_;
    e.draws_per_category = draws_per_category_;
    e.draws_ps_useless = draws_ps_useless_;
    e.draws_ps_needed = draws_ps_needed_;
    e.shadows_alpha_active = shadows_alpha_active_;
    e.shadows_alpha_off = shadows_alpha_off_;
    e.triangles_per_category = triangles_per_category_;
    e.passes_per_category = passes_per_category_;
    e.submits = submits_;
    e.submits_ubo = submits_ubo_;
    e.shared_observed = shared_observed_;
    e.shared_changed = shared_changed_;
    e.bytes_repeated_frame = bytes_repeated_frame_;
    e.bytes_equal_earlier = bytes_equal_earlier_;
    e.ns_hash_vertices = ns_hash_vertices_;
    e.causes.assign(causes_.begin(), causes_.end());
    e.stages_ns = stages_ns_;
    e.scene_with_discard = scene_with_discard_;
    e.scene_no_discard = scene_no_discard_;
    e.drawn_timed = drawn_timed_;
    e.vegetation_soon = draws_vegetation_soon_;
    std::sort(e.causes.begin(), e.causes.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return e;
  }

 private:
  bool Reject(uint32_t cause, const char* text) {
    ++rejected_;
    ++causes_[cause];
    last_cause_ = cause;
    Warn(cause, text);
    return false;
  }

  void Warn(uint32_t cause, const char* text) {
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[native] C6: {} (cause {})", text, cause);
    }
  }

  // --- Texture binds on a separate thread (bo2zm_native_texture_bind_thread) ----------------------------------
  /*
   * Why. With the memory pool, creating a texture does not ask the system for memory, but vkBindImageMemory
   * still makes two ioctls: reserving the plane's VA with its pte_kind and mapping the pool chunk into it
   * (nvk_image_plane_bind, nvk_image.c:1696-1710; vkCreateImage reserves no VA except for sparse images,
   * :1337-1350). All our textures are tiled with pte_kind GENERIC_16BX2 (nil/image.rs:439-440 and 876-918),
   * so both are always paid. In gameplay: 336 VA reservations in 95 ms and 336 mappings in 111 ms, ~0.6 ms of
   * wall time per texture with the ring asleep in the ioctl. Entering a new zone brings 33-45 textures in
   * one frame: 19-27 ms of stalled ring that did not show up in "textures X ms".
   *
   * How. The ring does the CPU part (vkCreateImage, requirements, pool chunk, descriptor slot) and only
   * queues the vkBindImageMemory. The draw is recorded with its slot and UploadTexture leaves the data in the
   * upload buffer as usual. In BeforeSubmit (CollectBindings) it waits for whatever is missing, creates the
   * views, writes the descriptors (UPDATE_AFTER_BIND: legal until submission) and records the barrier and
   * the copy in that same upload buffer, which goes in the same vkQueueSubmit, ahead of the work. The GPU
   * receives exactly the same thing in the same submission.
   *
   * The driver. vkBindImageMemory from another thread is safe: the VA and the mapping are under
   * device->va_mutex (horizon/nouveau_horizon_vm.c:173 and 377), the mapping refcount under
   * memory_identity_mutex (nouveau_horizon_memory.c:1065), and everything else belongs to that image. The
   * pool is not touched from the thread: it stays single-threaded (bo2zm_native_texture_pool.h).
   *
   * Threads (see docs/platform-notes.md, Threads). Shared state is under bindings_mutex_, and every decision
   * to sleep or wake is taken with the lock held; the thread only reads the image, memory and offset of its
   * request and stores the result under the lock. The ring's wait has a timeout and logs every 2 s.
   * Persistent thread (never detached), joined in the destructor before any image is destroyed. Priority
   * 0x2D: a host service, above the guest.
   *
   * Self-checking guard. Observing phase: the first kBindingsACheck wait for their bind right after
   * queuing it (same order as inline: only the thread doing the ioctl changes). If all succeed, applying
   * phase. In any phase, a failed bind, a wait of more than 2 s or a deferred copy that can no longer go in
   * its upload buffer: REXLOG_ERROR with the MISMATCH and off for the rest of the run (the usual path returns;
   * whatever was in flight finishes through the fallback path). And if for 3 reports in a row the ring
   * waits for the thread longer than the thread takes to bind, it does not pay off: it switches off too.
   */
  void DecideBindings() {
    const bool requested = REXCVAR_GET(bo2zm_native_texture_bind_thread);
    bindings_priority_ = std::clamp<int32_t>(REXCVAR_GET(bo2zm_native_texture_bind_thread_priority), 0x2C, 0x3B);
    bindings_phase_ = requested ? kBindingsObserving : kBindingsOff;
    REXLOG_INFO("[native] C3: binding new textures on a separate thread (bo2zm_native_texture_bind_thread) = {}",
                requested ? fmt::format("YES, priority {:#x}; OBSERVING phase: the first {} wait for their bind right "
                                     "away (pool textures only, which is {})",
                                     bindings_priority_, kBindingsACheck,
                                     pool_textures_.IsActive() ? "on" : "OFF: it will not be used")
                       : std::string("no, on the ring thread as always"));
  }

  void TurnOffBindings(const std::string& reason) {
    if (bindings_phase_ == kBindingsOff) {
      return;
    }
    bindings_phase_ = kBindingsOff;
    REXLOG_ERROR("[native] C3: texture bind thread OFF for the rest of the session: {}. New textures are created whole "
                 "on the ring thread again ({} bound on the thread, {} failed, {} views and {} copies postponed)",
                 reason, bindings_thread_total_, bindings_failed_, views_postponed_, copies_postponed_);
  }

  // Queues the vkBindImageMemory of an image. false = not queued (queue full or no thread): the usual path.
  bool EnqueueBinding(VkImage image, VkDeviceMemory guest_memory, VkDeviceSize offset, uint64_t& ticket) {
    if (!bindings_thread_.joinable()) {
      try {
        bindings_thread_ = std::thread([this] { LoopBindings(); });
      } catch (const std::system_error& error) {
        TurnOffBindings(fmt::format("MISMATCH: the thread could not be created ({})", error.what()));
        return false;
      }
    }
    bool warn = false;
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      if (bindings_requested_ - bindings_collected_ >= kQueueBindings) {
        ++bindings_queue_full_;
        return false;
      }
      BindRequest& request = queue_bindings_[bindings_requested_ & (kQueueBindings - 1)];
      request.image = image;
      request.guest_memory = guest_memory;
      request.offset = offset;
      request.result = VK_NOT_READY;
      request.ns = 0;
      ticket = bindings_requested_++;
      warn = bindings_sleeping_;  // decided under the lock: if it sleeps, it is inside its wait with the predicate
    }
    if (warn) {
      bindings_cv_.notify_one();
    }
    return true;
  }

  // The thread: the requests' vkBindImageMemory calls, in order. It sleeps on a decision taken under the
  // lock; it never spins. It does not write to the log (the ring records the figures in ReportBindings).
  void LoopBindings() {
    rex::thread::set_current_thread_name("BO2 texture binds");
#if REX_PLATFORM_SWITCH
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(bindings_priority_));
#else
    const bool priority_ok = true;
#endif
    std::unique_lock<std::mutex> lock(bindings_mutex_);
    bindings_priority_ok_ = priority_ok;
    for (;;) {
      if (bindings_done_ == bindings_requested_) {
        if (bindings_stop_) {
          return;  // stop, nothing pending
        }
        if (bindings_waiting_) {
          bindings_done_cv_.notify_one();
        }
        bindings_sleeping_ = true;
        bindings_cv_.wait(lock, [this] { return bindings_stop_ || bindings_done_ != bindings_requested_; });
        bindings_sleeping_ = false;
        continue;
      }
      // The slot is not reused until the ring collects it (bindings_collected_): it stays ours without the lock.
      BindRequest& request = queue_bindings_[bindings_done_ & (kQueueBindings - 1)];
      const VkImage image = request.image;
      const VkDeviceMemory guest_memory = request.guest_memory;
      const VkDeviceSize offset = request.offset;
      lock.unlock();
      const auto before = std::chrono::steady_clock::now();
      const VkResult result = dfn_.vkBindImageMemory(device_, image, guest_memory, offset);
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - before)
                                       .count());
      lock.lock();
      request.result = result;
      request.ns = ns;
      ns_bindings_thread_ += ns;
      ns_binding_worst_ = std::max(ns_binding_worst_, ns);
      ++bindings_done_;
      if (bindings_waiting_) {
        bindings_done_cv_.notify_one();
      }
    }
  }

  void StopBindings() {
    if (!bindings_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      bindings_stop_ = true;
    }
    bindings_cv_.notify_one();
    bindings_thread_.join();  // first finishes every queued request
    REXLOG_INFO("[native] C3: texture bind thread stopped ({} bound on the thread, {} failed, {} views and {} copies "
                "postponed, {} slots lost)",
                bindings_thread_total_, bindings_failed_, views_postponed_, copies_postponed_, slots_lost_);
  }

  // The ring waits until the thread has done the requests before `goal`. With a timeout and a log line
  // every 2 s.
  void WaitBindings(uint64_t goal) {
    std::unique_lock<std::mutex> lock(bindings_mutex_);
    if (bindings_done_ >= goal) {
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    bindings_waiting_ = true;
    if (bindings_sleeping_) {
      bindings_cv_.notify_one();  // should not be needed (it has work); in case a wake-up was lost
    }
    int seconds = 0;
    while (!bindings_done_cv_.wait_for(lock, std::chrono::seconds(2),
                                        [this, goal] { return bindings_done_ >= goal; })) {
      seconds += 2;
      bindings_stuck_ = true;
      REXLOG_ERROR("[native] C3: the ring thread has been waiting {} s for the texture binds: target {}, done {}, "
                   "requested {}; the thread is {}",
                   seconds, goal, bindings_done_, bindings_requested_,
                   bindings_sleeping_ ? "ASLEEP (wake-up lost)" : "awake (an ioctl that does not return)");
    }
    bindings_waiting_ = false;
    lock.unlock();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - before)
                                     .count());
    ns_wait_bindings_total_ += ns;
    ns_wait_bindings_report_ += ns;
    ns_wait_bindings_worst_ = std::max(ns_wait_bindings_worst_, ns);
    ++waits_bindings_report_;
    bo2zm::waits::g_ns_waiting_bindings.fetch_add(ns, std::memory_order_relaxed);
  }

  /*
   * The CPU part of CreateTexture (image, requirements, pool chunk) here, and its vkBindImageMemory to the
   * thread. false = nothing was touched and the caller continues through CreateTexture: thread off, pool off
   * or full, queue full, or (in the observing phase) a failed bind whose fallback path also failed.
   */
  bool CreateTextureInThread(CachedTexture& texture, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                          uint32_t depth3d, uint32_t levels) {
    if (bindings_phase_ == kBindingsNoDecide) {
      DecideBindings();
    }
    // With the out-of-memory test enabled, everything goes through CreateTexture: that test lives there.
    if (bindings_phase_ == kBindingsOff || !pool_textures_.IsActive() || test_no_memory_every_ > 0) {
      return false;
    }
    const VkImageCreateInfo info = InfoImageTexture(format, width, height, layers, depth3d, levels);
    VkImage image = VK_NULL_HANDLE;
    if (dfn_.vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    dfn_.vkGetImageMemoryRequirements(device_, image, &req);
    VkDeviceMemory block = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t id_block = 0xFFFFFFFFu;
    if (!pool_textures_.Reserve(req, block, offset, id_block)) {
      dfn_.vkDestroyImage(device_, image, nullptr);
      return false;  // no room in the pool: CreateTexture, which can take the dedicated path
    }
    uint64_t ticket = 0;
    if (!EnqueueBinding(image, block, offset, ticket)) {
      dfn_.vkDestroyImage(device_, image, nullptr);  // before the chunk, as the pool requires (not bound yet)
      pool_textures_.Free(id_block);
      return false;
    }
    texture.image.image = image;
    texture.image.guest_memory = VK_NULL_HANDLE;  // from the pool: not freed on its own
    texture.image.pool_block = id_block;
    texture.image.width = width;
    texture.image.height = height;
    texture.image.format = format;
    texture.image.prepared = false;
    TextureInFlight flight;
    flight.texture = &texture;
    flight.ticket = ticket;
    flight.image = image;
    flight.format = format;
    flight.width = width;
    flight.height = height;
    flight.layers = layers;
    flight.depth3d = depth3d;
    flight.levels = levels;
    in_flight_.push_back(flight);
    texture.in_flight = uint32_t(in_flight_.size());
    images_in_flight_.insert(image);
    if (bindings_phase_ == kBindingsObserving) {
      // Observing phase: wait now, before SlotView and UploadTexture: everything stays as without the thread.
      CollectBindings(true);
      if (texture.image.image == VK_NULL_HANDLE) {
        return false;  // neither the thread nor the fallback (already logged): CreateTexture, with its emergency path
      }
      if (++bindings_checked_ >= kBindingsACheck && bindings_phase_ == kBindingsObserving) {
        bindings_phase_ = kBindingsApplying;
        uint64_t ns_thread = 0;
        bool priority_ok = true;
        {
          std::lock_guard<std::mutex> lock(bindings_mutex_);
          ns_thread = ns_bindings_thread_;
          priority_ok = bindings_priority_ok_;
        }
        REXLOG_INFO("[native] C3: texture bind thread: {} binds with an immediate wait and none failed ({:.0f} us on "
                    "average inside vkBindImageMemory; the ring waited {:.0f} us on average per texture). Moves to the "
                    "APPLYING phase: the wait goes before each submit{}",
                    bindings_checked_, double(ns_thread) / 1e3 / double(bindings_checked_),
                    double(ns_wait_bindings_total_) / 1e3 / double(bindings_checked_),
                    priority_ok ? "" : " (the kernel did NOT accept the thread priority)");
      }
    }
    return true;
  }

  // SlotView for an image with its bind in flight: the slot now, the view in CollectBindings.
  uint32_t SlotViewInFlight(uint64_t key, VkImage image, VkFormat format, uint32_t swizzle,
                              uint16_t swizzle_host, uint32_t heap) {
    View view;
    view.image = image;
    view.heap = heap;
    view.slot = ReserveSlot(heap);
    if (!view.slot) {
      Warn(35, "texture heap full");
      ++slots_full_;
      return 0;
    }
    views_.emplace(key, view);  // vista.vista stays VK_NULL_HANDLE until CollectBindings
    views_per_image_[image].push_back(key);
    ViewInFlight postponed;
    postponed.key = key;
    postponed.image = image;
    postponed.format = format;
    postponed.swizzle = swizzle;
    postponed.swizzle_host = swizzle_host;
    postponed.heap = heap;
    postponed.slot = view.slot;
    views_in_flight_.push_back(postponed);
    ++views_postponed_;
    ++views_postponed_report_;
    return view.slot;
  }

  // UploadTexture for a texture with its bind in flight: the data is already in the upload buffer; record
  // where.
  bool PostponeCopy(CachedTexture& texture, VkDeviceSize offset, VkCommandBuffer upload) {
    const size_t index = size_t(texture.in_flight) - 1;
    if (index >= in_flight_.size() || in_flight_[index].texture != &texture || in_flight_[index].copy) {
      TurnOffBindings(fmt::format("MISMATCH: incoherent in-flight texture index ({} of {})", index,
                                in_flight_.size()));
      return false;
    }
    TextureInFlight& flight = in_flight_[index];
    flight.copy = true;
    flight.copy_offset = offset;
    flight.copy_epoch = epoch_upload_;
    flight.copy_commands = upload;
    ++copies_postponed_;
    ++copies_postponed_report_;
    return true;
  }

  // The deferred views of an image that had to be recreated: new key (it goes with the image) and its list.
  void MoveViewsInFlight(VkImage old, VkImage fresh) {
    if (old == fresh) {
      return;
    }
    std::vector<uint64_t> keys;
    for (ViewInFlight& postponed : views_in_flight_) {
      if (postponed.image != old) {
        continue;
      }
      const auto it = views_.find(postponed.key);
      if (it == views_.end()) {
        continue;
      }
      View view = it->second;
      views_.erase(it);
      view.image = fresh;
      postponed.image = fresh;
      if (fresh != VK_NULL_HANDLE) {
        postponed.key = KeyView(fresh, postponed.swizzle, postponed.swizzle_host, postponed.heap);
      }
      if (!views_.emplace(postponed.key, view).second) {
        postponed.key = 0;  // key collision: CollectBindings puts the empty one in its slot
        continue;
      }
      keys.push_back(postponed.key);
    }
    views_per_image_.erase(old);  // an image in flight only has deferred views
    if (fresh != VK_NULL_HANDLE && !keys.empty()) {
      auto& list = views_per_image_[fresh];
      list.insert(list.end(), keys.begin(), keys.end());
    }
  }

  /*
   * Finishes everything the bind thread has. Ring thread only. `with_copies` = false outside BeforeSubmit
   * (the upload buffer would no longer be the one holding that data): those textures are uploaded again at
   * their next check.
   */
  void CollectBindings(bool with_copies) {
    // No re-entry: if something in here ended up starting new work (UseSlot), that call does nothing and
    // the outer one sees the upload buffer changed (MISMATCH and a full upload at the next check).
    if (in_flight_.empty() || collecting_bindings_) {
      return;
    }
    collecting_bindings_ = true;
    const auto start = std::chrono::steady_clock::now();
    const uint64_t wait_before = ns_wait_bindings_total_;
    const uint64_t goal = in_flight_.back().ticket + 1;
    WaitBindings(goal);
    if (bindings_stuck_) {
      bindings_stuck_ = false;
      TurnOffBindings("MISMATCH: a wait of more than 2 s for the thread (see the lines above)");
    }
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      for (TextureInFlight& flight : in_flight_) {
        flight.result = queue_bindings_[flight.ticket & (kQueueBindings - 1)].result;
      }
      bindings_collected_ = goal;  // from here on its slots can be reused
    }
    // 1. Results, in order. If a bind failed, that image is dropped (no command references it yet) and the
    //    texture is created again through CreateTexture without the emergency path: releasing half the cache
    //    submits the work, and we are before that.
    for (TextureInFlight& flight : in_flight_) {
      CachedTexture& texture = *flight.texture;
      images_in_flight_.erase(flight.image);
      texture.in_flight = 0;
      if (flight.result == VK_SUCCESS) {
        ++bindings_thread_total_;
        ++bindings_thread_report_;
        bo2zm::waits::g_textures_bound_thread.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ++bindings_failed_;
      TurnOffBindings(fmt::format("MISMATCH: vkBindImageMemory returned {} on the thread for a {}x{} texture ({} "
                                "levels, {} layers)",
                                int32_t(flight.result), flight.width, flight.height, flight.levels, flight.layers));
      dfn_.vkDestroyImage(device_, flight.image, nullptr);
      pool_textures_.Free(texture.image.pool_block);
      texture.image.image = VK_NULL_HANDLE;
      texture.image.pool_block = 0xFFFFFFFFu;
      if (!CreateTexture(texture.image, flight.format, flight.width, flight.height, flight.layers, flight.depth3d,
                        flight.levels, false)) {
        // Not even then. The texture is left without an image: its draws in this submission see the empty one
        // (the same as when it cannot be created) and the next time it is used it is created in full, with the
        // emergency path.
        texture.image = NativeImage{};
        bytes_textures_ -= std::min(bytes_textures_, texture.bytes);
        texture.bytes = 0;
      }
      MoveViewsInFlight(flight.image, texture.image.image);
    }
    // 2. Deferred views: created and written to their slot. If that is not possible, the slot keeps the
    //    empty one (what a draw sees when SlotView returns 0) and is not reused: this submission
    //    references it (it is counted).
    for (const ViewInFlight& postponed : views_in_flight_) {
      const auto it = postponed.key ? views_.find(postponed.key) : views_.end();
      if (it != views_.end() && it->second.image != VK_NULL_HANDLE &&
          CreateViewTexture(it->second.image, postponed.format, postponed.swizzle, postponed.swizzle_host,
                            postponed.heap, it->second.view) == VK_SUCCESS) {
        WriteImage(postponed.heap, postponed.slot, it->second.view);
        continue;
      }
      WriteImage(postponed.heap, postponed.slot, empty_[postponed.heap].view);
      if (it != views_.end()) {
        views_.erase(it);  // the image has no views_per_image_ left (MoveViewsInFlight) or is rebuilt when used
      }
      ++slots_lost_;
      Warn(34, "could not create a texture's view");
    }
    views_in_flight_.clear();
    // 3. Deferred barriers and copies, in the upload buffer where UploadTexture left their data.
    VkCommandBuffer upload = VK_NULL_HANDLE;
    for (const TextureInFlight& flight : in_flight_) {
      if (!flight.copy) {
        continue;
      }
      CachedTexture& texture = *flight.texture;
      if (texture.image.image == VK_NULL_HANDLE) {
        continue;  // no image: created and fully uploaded next time it is used
      }
      if (with_copies && upload == VK_NULL_HANDLE) {
        upload = context_->CommandsUpload();
      }
      if (!with_copies || upload == VK_NULL_HANDLE || upload != flight.copy_commands ||
          flight.copy_epoch != epoch_upload_) {
        TurnOffBindings(fmt::format("MISMATCH: a postponed copy can no longer go into its upload buffer (epoch {} "
                                  "versus {}, {})",
                                  flight.copy_epoch, epoch_upload_,
                                  with_copies ? "another upload buffer" : "collected when changing buffer"));
        texture.image.prepared = false;  // the next check uploads it again in full, with its barrier
        texture.next = 0;
        continue;
      }
      if (!texture.image.prepared) {
        Barrier(upload, texture.image.image, texture.layers);
        texture.image.prepared = true;
      }
      RecordCopyTexture(upload, texture, flight.copy_offset);
    }
    in_flight_.clear();
    collecting_bindings_ = false;
    if (!measuring_creation_) {  // inside PrepareTexture its stopwatch already counts it
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
      const uint64_t wait = ns_wait_bindings_total_ - wait_before;
      const uint64_t own = ns > wait ? ns - wait : 0;
      bo2zm::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
      ns_create_report_ += own;
    }
  }

  // UseSlot: a texture with its bind in flight may reach new work (it was prepared before that work's
  // first Record), but not with its data already in the previous upload buffer: that should never happen.
  void CheckBindingsOnBufferChange() {
    for (const TextureInFlight& flight : in_flight_) {
      if (flight.copy) {
        CollectBindings(false);  // logs the MISMATCH, switches off and leaves those textures to be uploaded again
        return;
      }
    }
  }

  // Every 10 s, if there were new textures: what the thread did and how long the ring waited. This is also
  // where it is decided whether it pays off: if for 3 reports in a row the ring waits for the thread longer
  // than the thread takes to bind, the ring would do better doing it itself, and it switches off.
  // The 3D textures read again from guest memory (ReadVolume), the busiest first. Reading, untiling,
  // byte-swapping and uploading a 2 MB one every frame costs the console's ring thread ~8-11 ms per frame; this says
  // which ones are being refreshed.
  struct VolumeReadInfo {
    uint32_t base = 0, format = 0, width = 0, height = 0, depth3d = 0;
    uint64_t reads = 0, bytes = 0, ns = 0, already_prepared = 0;
  };
  std::unordered_map<uint64_t, VolumeReadInfo> volumes_read_;
  std::chrono::steady_clock::time_point report_volumes_{};
  void ReportVolumes() {
    const auto now = std::chrono::steady_clock::now();
    if (now - report_volumes_ < std::chrono::seconds(10)) {
      return;
    }
    report_volumes_ = now;
    if (volume_states_ || volume_no_changes_ || volume_updates_) {
      BO2ZM_REPORT_RING("[native] C3 3D textures by pages (bo2zm_native_incremental_volume), last 10 s: {} new states, "
                           "{} unchanged, {} updated ({} pages, {:.2f} MB uploaded), {:.1f} ms; {} checked against the "
                           "whole read since boot; finding the changed pages: comparing {:.0f} us each ({}), by hash "
                           "{:.0f} us each ({}), by the writer's marks {:.0f} us each ({}; {} also hashed as a check, "
                           "{} pages marked only after the check, {} pages changed without a mark)",
                           volume_states_, volume_no_changes_, volume_updated_report_, volume_pages_,
                           double(volume_bytes_) / 1048576.0, double(volume_ns_) / 1e6, volume_checked_,
                           volume_detects_[0] ? double(volume_detect_ns_[0]) / 1e3 / double(volume_detects_[0]) : 0.0,
                           volume_detects_[0],
                           volume_detects_[1] ? double(volume_detect_ns_[1]) / 1e3 / double(volume_detects_[1]) : 0.0,
                           volume_detects_[1],
                           volume_detects_[2] ? double(volume_detect_ns_[2]) / 1e3 / double(volume_detects_[2]) : 0.0,
                           volume_detects_[2], volume_mark_checked_, volume_mark_late_, volume_mark_misses_);
      volume_states_ = volume_no_changes_ = volume_updated_report_ = 0;
      volume_pages_ = volume_bytes_ = volume_ns_ = 0;
      volume_detect_ns_[0] = volume_detect_ns_[1] = volume_detects_[0] = volume_detects_[1] = 0;
      volume_detect_ns_[2] = volume_detects_[2] = volume_mark_checked_ = volume_mark_misses_ = volume_mark_late_ = 0;
    }
    if (volumes_read_.empty()) {
      return;
    }
    std::vector<VolumeReadInfo> list;
    for (const auto& [key, v] : volumes_read_) {
      list.push_back(v);
    }
    volumes_read_.clear();
    std::sort(list.begin(), list.end(), [](const VolumeReadInfo& a, const VolumeReadInfo& b) { return a.ns > b.ns; });
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(list.size(), 4); ++i) {
      const VolumeReadInfo& v = list[i];
      text += fmt::format("; {:08X} format {} {}x{}x{}: {} reads ({} of an already created one), {:.1f} MB, {:.1f} ms",
                           v.base, v.format, v.width, v.height, v.depth3d, v.reads, v.already_prepared,
                           double(v.bytes) / 1048576.0, double(v.ns) / 1e6);
    }
    BO2ZM_REPORT_RING("[native] C3 3D textures read from memory, last 10 s ({} different){}", list.size(),
                         text);
  }

  void ReportBindings() {
    if (bindings_phase_ == kBindingsNoDecide) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_bindings_ < std::chrono::seconds(10)) {
      return;
    }
    report_bindings_ = now;
    uint64_t ns_thread = 0;
    uint64_t worst_thread = 0;
    uint64_t queue_full = 0;
    bool priority_ok = true;
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      ns_thread = ns_bindings_thread_ - ns_bindings_thread_previous_;
      ns_bindings_thread_previous_ = ns_bindings_thread_;
      worst_thread = ns_binding_worst_;
      ns_binding_worst_ = 0;
      queue_full = bindings_queue_full_ - queue_full_previous_;
      queue_full_previous_ = bindings_queue_full_;
      priority_ok = bindings_priority_ok_;
    }
    const uint64_t created = textures_created_ - created_report_previous_;
    created_report_previous_ = textures_created_;
    const uint64_t slots_full = slots_full_ - slots_full_previous_;
    slots_full_previous_ = slots_full_;
    if (slots_full) {
      // Those textures were drawn with the empty (transparent black) texture.
      BO2ZM_REPORT_RING("[native] C3 texture slots: {} views found no slot in the last 10 s (drawn black); in use: 2D "
                        "{} of {}, 3D {} of {}, cube {} of {}",
                        slots_full, SlotsUsed(0), kCapacityHeap[0], SlotsUsed(1), kCapacityHeap[1], SlotsUsed(2),
                        kCapacityHeap[2]);
    }
    if (created || bindings_thread_report_ || waits_bindings_report_) {
      static constexpr const char* kPhases[] = {"off", "OBSERVING", "APPLYING"};
      BO2ZM_REPORT_RING(
          "[native] C3 new textures, last 10 s: {} created, {} with the bind on the thread; the ring spent "
          "{:.1f} ms creating and waited for the thread {:.1f} ms in {} waits (worst {:.2f} ms); the thread, {:.1f} ms "
          "inside vkBindImageMemory (average {:.0f} us, worst {:.2f} ms); {} views and {} copies postponed; {} by the "
          "usual path with the queue full; phase {}{}",
          created, bindings_thread_report_, double(ns_create_report_) / 1e6, double(ns_wait_bindings_report_) / 1e6,
          waits_bindings_report_, double(ns_wait_bindings_worst_) / 1e6, double(ns_thread) / 1e6,
          bindings_thread_report_ ? double(ns_thread) / 1e3 / double(bindings_thread_report_) : 0.0,
          double(worst_thread) / 1e6, views_postponed_report_, copies_postponed_report_, queue_full,
          kPhases[std::clamp<int32_t>(bindings_phase_, 0, 2)], priority_ok ? "" : " (the kernel did NOT accept the "
                                                                                  "priority)");
    }
    if (bindings_phase_ == kBindingsApplying && bindings_thread_report_ >= 16) {
      reports_no_compensate_ = ns_wait_bindings_report_ > ns_thread ? reports_no_compensate_ + 1 : 0;
      if (reports_no_compensate_ >= 3) {
        TurnOffBindings(fmt::format("not worth it: in 3 reports in a row the ring waited for the thread longer than the "
                                  "thread took to bind (the last one, {:.1f} ms of waiting for {:.1f} ms of binds)",
                                  double(ns_wait_bindings_report_) / 1e6, double(ns_thread) / 1e6));
      }
    }
    bindings_thread_report_ = 0;
    waits_bindings_report_ = 0;
    ns_wait_bindings_report_ = 0;
    ns_wait_bindings_worst_ = 0;
    ns_create_report_ = 0;
    views_postponed_report_ = 0;
    copies_postponed_report_ = 0;
  }

  // --- Hashing of new textures on a separate thread (bo2zm_native_texture_hash_thread) ------------------------
  /*
   * Why. During zone-change stutters the ring spends 5.3-8.7 ms of the frame in "textures": the XXH3 of
   * guest memory (2.3-3.1 ms, with some rechecks included) and the untiling and byte swapping of the new
   * ones (3.0-5.6 ms, 1.8-3.1 ms per MB); and outside that figure, the copy to the upload buffer (~0.4 ms
   * per MB). The ring is the bottleneck (the GPU waits 5-11 ms per frame).
   *
   * Exact. The hash and the untiling read guest memory, which the game may reuse as soon as the ring
   * returns the read pointer or writes a fence. So the thread is never given guest addresses: the ring
   * copies, right there (where the inline path computes the hash), the bytes the hash covers (base and
   * mips, the same ranges) into a snapshot, and the thread works on that copy. They are the same bytes read
   * at the same point, and the hash, the untiling and the byte swap are functions of those bytes: the
   * results are identical. Every read of the plan falls inside the copy (PlanReadHash; the bound was
   * tested against both untiling paths on 145,500 rectangles). The thread writes the result into the space
   * UploadTexture reserves in the upload buffer (same size, same order), and the barrier and the copy are
   * recorded where they always are: the GPU does not read that buffer until vkQueueSubmit, and
   * BeforeSubmit (CollectHashes) waits before that. Same submission, same bytes: no placeholder textures
   * and no frame of delay. The texture's hash_raw is set when collected, before anything looks at it.
   *
   * The ring never waits for more than one job. When collecting, it takes the ones the thread has not
   * started (from the end of the queue) and does them itself; it only waits for the one the thread is in
   * the middle of. If the thread has no core, the ring ends up doing the usual work at submission, plus the
   * copy, and the "not worth it" guard switches it off.
   *
   * Threads (see docs/platform-notes.md, Threads). All shared state is under hashes_mutex_, and every
   * decision to sleep or wake is taken with the lock held: no "store mine and read yours" with atomics.
   * The thread only reads the snapshot and the plan of its job (written before publishing it) and only
   * writes its buffers, its space in the upload buffer and its job's results; it does not write to the log.
   * Persistent thread (never detached), joined in the destructor before the upload buffer is released.
   * Priority 0x2E: the ring (0x2D) preempts it as soon as it has work (equal priorities do not time-slice)
   * and the guest (0x3B) does not starve it of a core; it also prefers a core other than the ring's, which
   * is busy.
   *
   * Self-checking guard. Observing phase: the first kHashesACheck new textures go through the usual
   * path (that is what gets uploaded) and also through the thread, which returns its raw hash and the XXH3
   * of its data; they are compared when collected. All equal: applying phase, and 1 in
   * kCheckHashOneOfEvery keeps being compared. A MISMATCH, a wait of more than 2 s, a job that does
   * not add up, or no payoff for 3 reports in a row: REXLOG_ERROR and off for the rest of the run. After a
   * MISMATCH, the thread's remaining textures are uploaded again in full. The worst case is the usual
   * path.
   */
  struct HashRead {  // one ReadLevel call, reading from the snapshot
    uint64_t source = 0;  // offset in snapshots_
    uint64_t row_bytes = 0;
    size_t target = 0;  // offset in the texture data
    uint32_t pitch_blocks = 0;
    uint32_t ox = 0;
    uint32_t oy = 0;
    uint32_t bx = 0;
    uint32_t by = 0;
    uint32_t bx_host = 0;
    bool tiled = false;
    bool check = false;  // the fast untiling guard repeats this level on the usual path
  };
  struct HashJob {
    // Written by the ring before publishing; whoever does the job (the thread or the ring) only reads it.
    CachedTexture* texture = nullptr;  // used by the ring only; in hashes_planned_, nullptr = already published or discarded
    uint64_t key = 0;
    uint64_t sequence = 0;
    uint64_t inst_base = 0;  // the copy of the base (all layers) in snapshots_
    uint64_t bytes_base = 0;
    uint64_t inst_mips = 0;  // the copy of the mips
    uint64_t bytes_mips = 0;
    size_t bytes_data = 0;
    uint8_t* target = nullptr;  // its space in the upload buffer; nullptr for comparison jobs
    uint64_t epoch = 0;          // epoch_upload_ of that buffer
    uint32_t read_start = 0;  // its plan: reads_hash_[read_start, read_start + reads)
    uint32_t reads = 0;
    uint32_t bytes_block = 1;
    uint32_t log2_block = 0;
    uint32_t unit_order = 0;
    uint32_t order = 0;
    uint32_t address = 0;  // for the log
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool fast = false;           // tiled_fast_ when planning
    bool check_order = false;  // the fast byte swap guard checks this texture
    bool compare = false;         // observing phase (or 1 in kCheckHashOneOfEvery): only returns its hashes
    bool warning_levels = false;    // collected without differences: the "levels checked ... all equal" line
    bool warning_orders = false;
    // Written by whoever does the job; the ring reads it under hashes_mutex_ once it is done.
    int32_t state = 0;
    bool per_ring = false;
    bool order_different = false;
    uint32_t levels_different = 0;
    std::array<uint32_t, 5> different{};  // first differing level: blocks in x and y, pitch and origin (x, y)
    uint64_t hash_raw = 0;
    uint64_t hash_data = 0;  // comparison jobs only
    uint64_t ns_hash = 0;
    uint64_t ns_rest = 0;
  };
  struct HashComparison {  // ring only: what the usual path produced for a comparison texture
    uint64_t raw_hash = 0;
    uint64_t data = 0;
    bool annotated = false;
  };
  static constexpr size_t kReadsPerTexture = 6 * 16;  // layers x mip levels
  static constexpr uint32_t kReadsHash = 8192;       // reads planned between two collections
  static constexpr uint64_t kQueueHashes = 256;           // power of 2
  static constexpr uint32_t kJobHashPublished = 0xFFFFFFFFu;  // in CachedTexture::hash_job
  static constexpr int32_t kJobPending = 1;
  static constexpr int32_t kJobInThread = 2;
  static constexpr int32_t kJobInRing = 3;
  static constexpr int32_t kJobDone = 4;
  static constexpr int32_t kHashesNoDecide = -1;
  static constexpr int32_t kHashesOff = 0;
  static constexpr int32_t kHashesObserving = 1;
  static constexpr int32_t kHashesApplying = 2;
  static constexpr uint64_t kHashesACheck = 64;
  static constexpr uint64_t kCheckHashOneOfEvery = 128;

  void DecideHashes() {
    const bool requested = REXCVAR_GET(bo2zm_native_texture_hash_thread);
    hashes_priority_ = std::clamp<int32_t>(REXCVAR_GET(bo2zm_native_texture_hash_thread_priority), 0x2C, 0x3B);
    hashes_core_requested_ = std::clamp<int32_t>(REXCVAR_GET(bo2zm_native_texture_hash_thread_core), -2, 2);
    snapshots_bytes_ = uint64_t(std::clamp<int32_t>(REXCVAR_GET(bo2zm_native_texture_hash_thread_mb), 4, 64))
                          << 20;
    hashes_phase_ = requested ? kHashesObserving : kHashesOff;
    REXLOG_INFO("[native] C3: hashing and untiling of new textures on a separate thread "
                "(bo2zm_native_texture_hash_thread) = {}",
                requested ? fmt::format("YES, priority {:#x}, {} MB of snapshots; OBSERVING phase: the first {} also go "
                                     "by the usual path and are compared",
                                     hashes_priority_, snapshots_bytes_ >> 20, kHashesACheck)
                       : std::string("no, on the ring thread as always"));
  }

  void TurnOffHashes(const std::string& reason) {
    if (hashes_phase_ == kHashesOff) {
      return;
    }
    hashes_phase_ = kHashesOff;
    REXLOG_ERROR("[native] C3: texture hash thread OFF for the rest of the session: {}. New textures are prepared whole "
                 "on the ring thread again ({} done by the thread, {} by the ring, {} compared)",
                 reason, hashes_thread_total_, hashes_ring_total_, hashes_compared_);
  }

  /*
   * One ReadLevel call turned into a read from the snapshot. false = the texture goes through the usual
   * path: ReadLevel would go outside memory (its own check) or would read outside what the hash covers.
   * `range_*` is the hash region (base or mips) and `in_copy` is where its copy starts in the snapshot.
   * The bound on what is read is the hash's (GetTiledAddressUpperBound2D): the corner of the last
   * 32x32-block tile plus the size of one tile; it was checked against what UntileLevel and
   * ReadTiledLevelUsual read.
   */
  static bool PlanReadHash(std::array<HashRead, kReadsPerTexture>& reads, uint32_t& n,
                                   uint64_t address, bool tiled, uint32_t pitch_blocks, uint64_t row_bytes,
                                   const HostTextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                                   uint32_t bx_host, size_t target, uint64_t range_start, uint64_t range_bytes,
                                   uint64_t in_copy) {
    if (!bx || !by) {
      return true;  // ReadLevel reads nothing
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t slack =
        tiled ? uint64_t(std::max<int64_t>(OffsetTiled2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u), pitch_blocks,
                                                                     log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + slack > kMemoryPhysical) {
      return false;  // ReadLevel would return false: let the usual path report it
    }
    const uint64_t read =
        tiled ? uint64_t(std::max<int64_t>(OffsetTiled2D(int32_t((ox + bx - 1) & ~31u),
                                                                     int32_t((oy + by - 1) & ~31u), pitch_blocks,
                                                                     log2),
                                             0)) +
                      (log2 == 0 ? 0xA00u : log2 == 1 ? 0xC00u : (0x400u << log2))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (n >= reads.size() || address < range_start || address + read > range_start + range_bytes) {
      return false;
    }
    HashRead& l = reads[n++];
    l.source = in_copy + (address - range_start);
    l.row_bytes = row_bytes;
    l.target = target;
    l.pitch_blocks = pitch_blocks;
    l.ox = ox;
    l.oy = oy;
    l.bx = bx;
    l.by = by;
    l.bx_host = bx_host;
    l.tiled = tiled;
    l.check = false;
    return true;
  }

  /*
   * The rest of a new texture for the thread: space, snapshot, plan and job. true = applying phase: the
   * thread prepares it and the tail of PrepareTexture is done here (interval, upload and counters); the
   * caller returns. false = the usual path continues: it was not possible, or it is a comparison texture
   * (observing phase, and 1 in kCheckHashOneOfEvery), whose job is already published and is compared
   * when collected.
   */
  bool PlanHash(CachedTexture& texture, uint64_t key, uint32_t n_reads, const uint8_t* raw_base,
                     uint64_t extension, const uint8_t* raw_mips, uint64_t extension_mips, const HostTextureFormat& tf,
                     uint32_t order, size_t bytes_data, const std::array<size_t, 16>& offsets,
                     uint32_t address, uint32_t width, uint32_t height, uint32_t format, VkDeviceSize& bytes_upload) {
    comparison_texture_ = nullptr;
    if (hashes_phase_ == kHashesNoDecide) {
      DecideHashes();
    }
    if (hashes_phase_ == kHashesOff || texture.hash_job != 0 ||
        (hashes_phase_ == kHashesObserving &&
         hashes_comparisons_planned_ >= kHashesACheck + hashes_no_compare_)) {
      return false;  // while observing, with comparisons already running, the rest only take the usual path
    }
    if (!snapshots_) {
      snapshots_.reset(new (std::nothrow) uint8_t[size_t(snapshots_bytes_)]);
      reads_hash_.reset(new (std::nothrow) HashRead[kReadsHash]);
      if (!snapshots_ || !reads_hash_) {
        snapshots_.reset();
        reads_hash_.reset();
        TurnOffHashes(fmt::format("MISMATCH: there is no memory for {} MB of snapshots", snapshots_bytes_ >> 20));
        return false;
      }
    }
    const uint64_t inst_base = (snapshots_used_ + 63) & ~uint64_t(63);
    const uint64_t inst_mips = inst_base + ((extension + 63) & ~uint64_t(63));  // the same space the plan assumes
    const uint64_t in_queue = hashes_published_ - hashes_collected_ + hashes_planned_pending_;
    if (in_queue >= kQueueHashes || reads_used_ + n_reads > kReadsHash ||
        inst_mips + extension_mips > snapshots_bytes_) {
      ++hashes_no_room_report_;
      return false;
    }
    const bool compare =
        hashes_phase_ == kHashesObserving || (++hashes_new_applying_ % kCheckHashOneOfEvery) == 0;
    // The snapshot: the same bytes the usual path's hash covers, at the same point.
    const auto before_copy = std::chrono::steady_clock::now();
    std::memcpy(snapshots_.get() + inst_base, raw_base, size_t(extension));
    if (extension_mips) {
      std::memcpy(snapshots_.get() + inst_mips, raw_mips, size_t(extension_mips));
    }
    const uint64_t ns_copy = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before_copy)
                                           .count());
    snapshots_used_ = inst_mips + extension_mips;
    ns_snapshots_report_ += ns_copy;
    bytes_snapshots_report_ += extension + extension_mips;
    bo2zm::waits::g_ns_hash_snapshot.fetch_add(ns_copy, std::memory_order_relaxed);
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    HashJob t;
    t.texture = &texture;
    t.key = key;
    t.inst_base = inst_base;
    t.bytes_base = extension;
    t.inst_mips = inst_mips;
    t.bytes_mips = extension_mips;
    t.bytes_data = bytes_data;
    t.read_start = reads_used_;
    t.reads = n_reads;
    t.bytes_block = tf.bytes;
    t.log2_block = log2;
    t.unit_order = tf.unit_order;
    t.order = order;
    t.address = address;
    t.width = width;
    t.height = height;
    t.format = format;
    t.fast = tiled_fast_ > 0;
    t.compare = compare;
    // The plan, with the decisions of the fast untiling and byte swap guards taken in the same order as the
    // usual path (levels_seen_ and orders_seen_). Not for a comparison texture: the usual path also
    // processes it, and that is what keeps those counts.
    for (uint32_t i = 0; i < n_reads; ++i) {
      HashRead l = reads_plan_[i];
      l.source += inst_base;
      if (!compare && l.tiled && t.fast && tf.bytes == (1u << log2)) {
        ++levels_seen_;
        l.check = levels_seen_ <= kLevelsACheck || levels_seen_ % kCheckOneOfEvery == 0;
        if (l.check && ++levels_checked_ == kLevelsACheck) {
          t.warning_levels = true;
        }
      }
      reads_hash_[reads_used_ + i] = l;
    }
    reads_used_ += n_reads;
    if (!compare && t.fast && order != 0 && (tf.unit_order == 2 || tf.unit_order == 4)) {
      ++orders_seen_;
      t.check_order = orders_seen_ <= kLevelsACheck || orders_seen_ % kCheckOneOfEvery == 0;
      if (t.check_order && ++orders_checked_ == kLevelsACheck) {
        t.warning_orders = true;
      }
    }
    ++hashes_planned_report_;
    if (compare) {
      // Published now, with no space in the upload buffer: the thread only returns its two hashes. What gets
      // uploaded is the usual path's result, which continues on return (AnnotateComparisonHash stores its
      // hashes).
      ++hashes_comparisons_planned_;
      ++hashes_comparison_report_;
      const uint64_t sequence = PublishJobHash(t, nullptr);
      comparisons_hash_[sequence & (kQueueHashes - 1)] = HashComparison{};
      comparison_texture_ = &texture;
      comparison_sequence_ = sequence;
      return false;
    }
    // Applying phase: the tail of PrepareTexture for a new texture, the same as there.
    hashes_planned_.push_back(t);
    ++hashes_planned_pending_;
    texture.hash_job = uint32_t(hashes_planned_.size());
    texture.sample_valid = false;
    texture.samples_consecutive = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      texture.offset_level[n] = uint32_t(offsets[n]);
    }
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.hash = 0;  // a new texture's data hash decides nothing: 0, as on the usual path
    texture.needs_upload = true;
    textures_a_upload_.push_back(&texture);
    bo2zm::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    bo2zm::waits::g_bytes_uploaded.fetch_add(bytes_data, std::memory_order_relaxed);
    bytes_upload += (bytes_data + 3) & ~size_t(3);
    return true;
  }

  // Queues a job for the thread (under the lock), waking it if it sleeps. Returns its sequence number. If
  // the thread cannot be created, the job stays queued anyway and the ring does it when collecting (and the
  // feature switches off).
  uint64_t PublishJobHash(const HashJob& job, uint8_t* target) {
    if (!hashes_thread_.joinable() && !hashes_no_thread_) {
#if REX_PLATFORM_SWITCH
      const int ring = RexSwitchCurrentCore();  // the ring thread asks for itself
#else
      const int ring = -1;
#endif
      hashes_core_ = hashes_core_requested_ != -2 ? hashes_core_requested_ : ring == 2 ? 1 : 2;
      try {
        hashes_thread_ = std::thread([this] { LoopHashes(); });
        REXLOG_INFO("[native] C3: texture hash thread created: preferred core {} (the ring runs on {}), priority {:#x}",
                    hashes_core_, ring, hashes_priority_);
      } catch (const std::system_error& error) {
        hashes_no_thread_ = true;
        TurnOffHashes(fmt::format("MISMATCH: the thread could not be created ({})", error.what()));
      }
    }
    bool warn = false;
    uint64_t sequence = 0;
    {
      std::lock_guard<std::mutex> lock(hashes_mutex_);
      sequence = hashes_published_;
      HashJob& cell = queue_hashes_[sequence & (kQueueHashes - 1)];
      cell = job;
      cell.sequence = sequence;
      cell.target = target;
      cell.epoch = epoch_upload_;
      cell.state = kJobPending;
      ++hashes_published_;
      warn = hashes_sleeping_;  // decided under the lock: if it sleeps, it is in its wait with the predicate
    }
    if (warn) {
      hashes_cv_.notify_one();
    }
    return sequence;
  }

  // The data bytes of a planned texture (UploadTexture reserves that space). If its index does not match
  // (should not happen), the texture's own, which come from the same per-level computation.
  size_t BytesHashPlanned(const CachedTexture& texture) const {
    const size_t index = size_t(texture.hash_job) - 1;
    return index < hashes_planned_.size() && hashes_planned_[index].texture == &texture
               ? hashes_planned_[index].bytes_data
               : size_t(texture.bytes);
  }

  // UploadTexture for a planned texture: its job, with the space just reserved for it, goes to the thread's
  // queue.
  void PublishHash(CachedTexture& texture, uint8_t* target, size_t bytes) {
    const size_t index = size_t(texture.hash_job) - 1;
    if (index >= hashes_planned_.size() || hashes_planned_[index].texture != &texture) {
      // Should never happen. Without a job its data would never arrive: zeros in its space, and it is uploaded
      // in full next time.
      std::memset(target, 0, bytes);
      texture.hash_job = 0;
      texture.hash_raw = 0;
      texture.next = 0;
      TurnOffHashes(fmt::format("MISMATCH: incoherent planned job ({} of {})", index,
                                hashes_planned_.size()));
      return;
    }
    PublishJobHash(hashes_planned_[index], target);
    hashes_planned_[index].texture = nullptr;  // published: no longer counts as planned
    --hashes_planned_pending_;
    texture.hash_job = kJobHashPublished;
  }

  // At the start of each draw: the previous draw's planned textures that never reached UploadTexture (the
  // draw was rejected before). As on the usual path, they stay un-uploaded and are prepared again at their
  // next check.
  void DiscardHashesPlanned() {
    if (!hashes_planned_pending_) {
      return;
    }
    for (HashJob& t : hashes_planned_) {
      if (t.texture) {
        t.texture->hash_job = 0;
        t.texture = nullptr;
        ++hashes_discarded_;
      }
    }
    hashes_planned_pending_ = 0;
  }

  // Observing phase (and 1 in kCheckHashOneOfEvery): the usual path's hashes of the texture whose
  // comparison job was just published. CollectHashes compares them.
  void AnnotateComparisonHash(const CachedTexture& texture, const std::vector<uint8_t>& data) {
    comparison_texture_ = nullptr;
    if (comparison_sequence_ < hashes_collected_) {
      return;  // already collected (should not happen): left uncompared and counted
    }
    HashComparison& c = comparisons_hash_[comparison_sequence_ & (kQueueHashes - 1)];
    c.raw_hash = texture.hash_raw;
    c.data = XXH3_64bits(data.data(), data.size());
    c.annotated = true;
  }

  /*
   * One job: done by the thread (or by the ring, if it takes it when collecting). The same as
   * PrepareTexture's usual path and in the same order (raw hash, levels through ReadLevel, and byte swap
   * with its guard), but reading from the snapshot. It only reads this job's snapshot and plan, and only
   * writes `data`, `check_index`, its space in the upload buffer and the results in `t`: nothing of the
   * ring's state. `fast_off` belongs to whoever does the job: after a difference in its guard, the
   * rest takes the usual path, as on the ring (which also turns off tiled_fast_ when collecting it).
   */
  void ExecuteHash(HashJob& t, std::vector<uint8_t>& data, std::vector<uint8_t>& check_index,
                      bool& fast_off) {
    const auto start = std::chrono::steady_clock::now();
    const uint8_t* const snapshot = snapshots_.get();
    uint64_t hash = XXH3_64bits(snapshot + t.inst_base, size_t(t.bytes_base));
    if (t.bytes_mips) {
      hash = XXH3_64bits_withSeed(snapshot + t.inst_mips, size_t(t.bytes_mips), hash);
    }
    t.hash_raw = hash;
    const auto after_hash = std::chrono::steady_clock::now();
    data.assign(t.bytes_data, 0);
    const uint32_t b = t.bytes_block;
    for (uint32_t i = 0; i < t.reads; ++i) {
      const HashRead& l = reads_hash_[t.read_start + i];
      const uint8_t* const source = snapshot + l.source;
      uint8_t* const target = data.data() + l.target;
      if (!l.tiled) {
        for (uint32_t y = 0; y < l.by; ++y) {
          std::memcpy(target + size_t(y) * l.bx_host * b,
                      source + uint64_t(l.oy + y) * l.row_bytes + uint64_t(l.ox) * b, size_t(l.bx) * b);
        }
        continue;
      }
      if (t.fast && !fast_off && b == (1u << t.log2_block)) {
        switch (t.log2_block) {
          case 0: UntileLevel<0>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 1: UntileLevel<1>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 2: UntileLevel<2>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 3: UntileLevel<3>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          default: UntileLevel<4>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
        }
        if (l.check) {
          // The ReadLevel guard: repeated on the usual path (which is the result kept) and compared.
          check_index.assign(size_t(l.by) * l.bx_host * b, 0);
          for (uint32_t y = 0; y < l.by; ++y) {
            std::memcpy(check_index.data() + size_t(y) * l.bx_host * b, target + size_t(y) * l.bx_host * b,
                        size_t(l.bx) * b);
          }
          ReadTiledLevelUsual(source, l.pitch_blocks, t.log2_block, b, l.ox, l.oy, l.bx, l.by, l.bx_host,
                                    target);
          bool equal = true;
          for (uint32_t y = 0; y < l.by && equal; ++y) {
            equal = std::memcmp(check_index.data() + size_t(y) * l.bx_host * b,
                                  target + size_t(y) * l.bx_host * b, size_t(l.bx) * b) == 0;
          }
          if (!equal) {
            if (!t.levels_different++) {
              t.different = {l.bx, l.by, l.pitch_blocks, l.ox, l.oy};
            }
            fast_off = true;
          }
        }
        continue;
      }
      ReadTiledLevelUsual(source, l.pitch_blocks, t.log2_block, b, l.ox, l.oy, l.bx, l.by, l.bx_host, target);
    }
    const auto order = static_cast<xenos::Endian>(t.order);
    if (t.fast && !fast_off && order != xenos::Endian::kNone && (t.unit_order == 2 || t.unit_order == 4)) {
      if (t.check_order) {
        check_index = data;
      }
      ChangeOrderBytes(data.data(), data.size(), t.unit_order, t.order);
      if (t.check_order) {
        UsualOrder(check_index, t.unit_order, order);
        if (check_index != data) {
          t.order_different = true;
          data.swap(check_index);  // the usual one is kept, as on the ring
          fast_off = true;
        }
      }
    } else {
      UsualOrder(data, t.unit_order, order);
    }
    if (t.compare) {
      t.hash_data = XXH3_64bits(data.data(), data.size());
    }
    if (t.target) {
      std::memcpy(t.target, data.data(), data.size());
    }
    const auto end = std::chrono::steady_clock::now();
    t.ns_hash = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_hash - start).count());
    t.ns_rest = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - after_hash).count());
  }

  // The thread: the queued jobs, in order, skipping those the ring has taken. It sleeps on a decision taken
  // under the lock; it never spins. It does not write to the log (the ring records the figures when
  // collecting).
  void LoopHashes() {
    rex::thread::set_current_thread_name("BO2 texture hashes");
#if REX_PLATFORM_SWITCH
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(hashes_priority_));
    const bool core_ok = RexSwitchSetCurrentThreadCore(int(hashes_core_));
    const int core = RexSwitchCurrentCore();
#else
    const bool priority_ok = true;
    const bool core_ok = true;
    const int core = -1;
#endif
    std::vector<uint8_t> data;         // owned by this thread only
    std::vector<uint8_t> check_index;  // owned by this thread only
    bool fast_off = false;        // owned by this thread only: its fast-path guard saw a difference
    std::unique_lock<std::mutex> lock(hashes_mutex_);
    hashes_priority_ok_ = priority_ok;
    hashes_core_ok_ = core_ok;
    hashes_core_real_ = core;
    for (;;) {
      while (hashes_taken_ != hashes_published_ &&
             queue_hashes_[hashes_taken_ & (kQueueHashes - 1)].state != kJobPending) {
        ++hashes_taken_;  // taken by the ring
      }
      if (hashes_taken_ == hashes_published_) {
        if (hashes_stop_) {
          return;  // stop, nothing pending
        }
        if (hashes_waiting_) {
          hashes_done_cv_.notify_one();
        }
        hashes_sleeping_ = true;
        hashes_cv_.wait(lock, [this] { return hashes_stop_ || hashes_taken_ != hashes_published_; });
        hashes_sleeping_ = false;
        continue;
      }
      // The slot belongs to this thread while it is EnHilo: the ring neither touches nor reuses it until it is
      // collected done.
      HashJob& t = queue_hashes_[hashes_taken_ & (kQueueHashes - 1)];
      t.state = kJobInThread;
      ++hashes_taken_;
      lock.unlock();
      ExecuteHash(t, data, check_index, fast_off);
      lock.lock();
      t.state = kJobDone;
      ++hashes_done_;
      if (hashes_waiting_) {
        hashes_done_cv_.notify_one();
      }
    }
  }

  /*
   * Waits for and collects all published jobs. Ring thread only: in BeforeSubmit (before vkQueueSubmit)
   * and, if needed, before rechecking a texture. It takes the ones the thread has not started (from the
   * end) and does them itself; it only waits for the one the thread is in the middle of, with a timeout and
   * a log line every 2 s. Then it sets each texture's raw hash and compares the comparison jobs. If nothing
   * planned is left, the snapshots start over.
   */
  void CollectHashes() {
    if (collecting_hashes_) {
      return;
    }
    if (hashes_published_ == hashes_collected_) {
      if (!hashes_planned_pending_ && (snapshots_used_ || reads_used_)) {
        snapshots_used_ = 0;  // nothing published uncollected or planned: everything starts over
        reads_used_ = 0;
        hashes_planned_.clear();
      }
      return;
    }
    collecting_hashes_ = true;
    uint64_t ns_wait = 0;
    uint64_t ns_ring = 0;
    uint64_t done_ring = 0;
    {
      std::unique_lock<std::mutex> lock(hashes_mutex_);
      int seconds = 0;
      while (hashes_done_ != hashes_published_) {
        // 1. One the thread has not started, from the end of the queue (the thread goes from the start).
        HashJob* mine = nullptr;
        for (uint64_t i = hashes_published_; i > hashes_taken_; --i) {
          HashJob& t = queue_hashes_[(i - 1) & (kQueueHashes - 1)];
          if (t.state == kJobPending) {
            t.state = kJobInRing;
            mine = &t;
            break;
          }
        }
        if (mine) {
          lock.unlock();
          const auto before = std::chrono::steady_clock::now();
          ExecuteHash(*mine, hashes_data_ring_, hashes_check_ring_, hashes_fast_off_ring_);
          ns_ring += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count());
          ++done_ring;
          lock.lock();
          mine->per_ring = true;
          mine->state = kJobDone;
          ++hashes_done_;
          continue;
        }
        // 2. What is left, the thread has half done: wait for it, deciding under the lock.
        const auto before = std::chrono::steady_clock::now();
        hashes_waiting_ = true;
        if (hashes_sleeping_) {
          hashes_cv_.notify_one();  // should not be needed (it has one in progress); in case a wake-up was lost
        }
        const bool ready = hashes_done_cv_.wait_for(lock, std::chrono::seconds(2),
                                                         [this] { return hashes_done_ == hashes_published_; });
        hashes_waiting_ = false;
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before)
                                         .count());
        ns_wait += ns;
        ns_wait_hashes_worst_ = std::max(ns_wait_hashes_worst_, ns);
        ++waits_hashes_report_;
        if (!ready) {
          seconds += 2;
          hashes_stuck_ = true;
          REXLOG_ERROR("[native] C3: the ring thread has been waiting {} s for the texture hash thread: published {}, "
                       "taken {}, done {}; the thread is {}",
                       seconds, hashes_published_, hashes_taken_, hashes_done_,
                       hashes_sleeping_ ? "ASLEEP (wake-up lost)" : "awake (a job that does not finish)");
        }
      }
      // The results were written by whoever did each job; the lock makes them visible here.
      collected_hash_jobs_.clear();
      for (uint64_t s = hashes_collected_; s < hashes_published_; ++s) {
        collected_hash_jobs_.push_back(queue_hashes_[s & (kQueueHashes - 1)]);
      }
      hashes_collected_ = hashes_published_;  // from here on its slots can be reused
    }
    comparison_texture_ = nullptr;
    bool mismatch = false;
    std::string detail;
    uint64_t thread = 0;
    uint64_t ns_thread = 0;
    for (HashJob& t : collected_hash_jobs_) {
      if (t.per_ring) {
        ++hashes_ring_total_;
      } else {
        ++hashes_thread_total_;
        if (!t.compare) {  // what the thread actually took off the ring
          ++thread;
          ns_thread += t.ns_hash + t.ns_rest;
          ns_hash_raw_thread_report_ += t.ns_hash;
        }
      }
      // The fast-path guards, with the same lines as the usual path (ReadLevel and the byte swap).
      if (t.levels_different) {
        REXLOG_ERROR("[native] C3: the fast untiling does NOT match ({}x{} blocks of {} bytes, pitch {}, from {},{}). "
                     "It is switched OFF and the usual one is used.",
                     t.different[0], t.different[1], t.bytes_block, t.different[2], t.different[3], t.different[4]);
        tiled_fast_ = 0;
      } else if (t.warning_levels) {
        REXLOG_INFO("[native] C3: fast untiling: {} levels checked against the usual one, all equal. The fast one "
                    "stays.",
                    kLevelsACheck);
      }
      if (t.order_different) {
        REXLOG_ERROR("[native] C3: the fast byte-order change does NOT match (unit {}, order {}, {} bytes). The fast "
                     "byte order and untiling are switched OFF.",
                     t.unit_order, t.order, t.bytes_data);
        tiled_fast_ = 0;
      } else if (t.warning_orders) {
        REXLOG_INFO("[native] C3: fast byte-order change: {} textures checked against the usual one, all equal.",
                    kLevelsACheck);
      }
      if (t.compare) {
        const HashComparison& c = comparisons_hash_[t.sequence & (kQueueHashes - 1)];
        if (!c.annotated) {
          ++hashes_no_compare_;
          continue;
        }
        ++hashes_compared_;
        if (c.raw_hash != t.hash_raw || c.data != t.hash_data) {
          mismatch = true;
          detail = fmt::format("MISMATCH in texture {:08X} {}x{} format {}: raw hash {:016X} on the ring and {:016X} on "
                                "the thread; data {:016X} and {:016X}",
                                t.address, t.width, t.height, t.format, c.raw_hash, t.hash_raw, c.data,
                                t.hash_data);
        }
        continue;
      }
      // A texture done by the thread: its raw hash is that of the same bytes that were uploaded. It must still
      // be in the cache as it was left, and its data must have gone to the current upload buffer (the one about
      // to be submitted).
      const auto it = textures_.find(t.key);
      if (it == textures_.end() || &it->second != t.texture || t.texture->hash_job != kJobHashPublished) {
        mismatch = true;
        detail = fmt::format("MISMATCH: texture {:08X} of a collected job is no longer as it was left",
                              t.address);
        t.texture = nullptr;
        continue;
      }
      if (t.epoch != epoch_upload_) {
        mismatch = true;  // uploaded again in full (hashes_suspicious_)
        detail = fmt::format("MISMATCH: texture {:08X}'s data went to another upload buffer (epoch {} versus {})",
                              t.address, t.epoch, epoch_upload_);
      }
      t.texture->hash_raw = t.hash_raw;
      t.texture->hash_job = 0;
      AnnotateContentTexture(*t.texture, t.key);  // measurement only: bo2zm_native_diag_reuse
    }
    if (mismatch) {
      hashes_suspicious_ = true;
      TurnOffHashes(detail);
    }
    if (hashes_stuck_) {
      hashes_stuck_ = false;
      TurnOffHashes("MISMATCH: a wait of more than 2 s for the hash thread (see the lines above)");
    }
    if (hashes_suspicious_) {
      // The thread's textures planned before the MISMATCH may carry the same fault: they are uploaded again
      // in full, on the usual path (already switched off), at their next check.
      for (const HashJob& t : collected_hash_jobs_) {
        if (!t.compare && t.texture) {
          t.texture->image.prepared = false;
          t.texture->next = 0;
          t.texture->hash_job = 0;
          ++hashes_reuploads_;
        }
      }
    } else if (hashes_phase_ == kHashesObserving && hashes_compared_ >= kHashesACheck) {
      hashes_phase_ = kHashesApplying;
      bool priority_ok = true;
      bool core_ok = true;
      int core = -1;
      {
        std::lock_guard<std::mutex> lock(hashes_mutex_);
        priority_ok = hashes_priority_ok_;
        core_ok = hashes_core_ok_;
        core = hashes_core_real_;
      }
      REXLOG_INFO("[native] C3: texture hash thread: {} new textures done by both paths, all equal (raw hash and data; "
                  "{} could not be compared). Moves to the APPLYING phase: the thread prepares them and 1 in {} is "
                  "still compared. The thread runs on core {}{}{}",
                  hashes_compared_, hashes_no_compare_, kCheckHashOneOfEvery, core,
                  core_ok ? "" : " (the kernel did NOT accept the preferred core)",
                  priority_ok ? "" : " (the kernel did NOT accept the priority)");
    }
    hashes_thread_report_ += thread;
    hashes_ring_report_ += done_ring;
    ns_hashes_thread_report_ += ns_thread;
    ns_hashes_ring_report_ += ns_ring;
    ns_wait_hashes_report_ += ns_wait;
    // A few times per frame (on each submission with new textures): the fetch_add calls do not show.
    bo2zm::waits::g_textures_hash_thread.fetch_add(thread, std::memory_order_relaxed);
    bo2zm::waits::g_ns_hash_thread.fetch_add(ns_thread, std::memory_order_relaxed);
    bo2zm::waits::g_textures_hash_ring.fetch_add(done_ring, std::memory_order_relaxed);
    bo2zm::waits::g_ns_hash_ring.fetch_add(ns_ring, std::memory_order_relaxed);
    bo2zm::waits::g_ns_waiting_hashes.fetch_add(ns_wait, std::memory_order_relaxed);
    if (!hashes_planned_pending_) {
      snapshots_used_ = 0;
      reads_used_ = 0;
      hashes_planned_.clear();
    }
    collecting_hashes_ = false;
  }

  // UseSlot: everything published is collected in BeforeSubmit. If anything reaches here (it should
  // not), its data was going to the buffer just closed without waiting for it: it is collected now (before
  // this one is reset) and those textures are uploaded again.
  void CheckHashesOnBufferChange() {
    if (hashes_published_ == hashes_collected_) {
      return;
    }
    const uint64_t no_collect = hashes_published_ - hashes_collected_;
    hashes_suspicious_ = true;  // whatever is collected now is uploaded again in full
    CollectHashes();
    TurnOffHashes(fmt::format("MISMATCH: {} hash thread jobs not collected when the upload buffer changed",
                              no_collect));
  }

  void StopHashes() {
    if (!hashes_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(hashes_mutex_);
      hashes_stop_ = true;
    }
    hashes_cv_.notify_one();
    hashes_thread_.join();  // first finishes the published jobs
    REXLOG_INFO("[native] C3: texture hash thread stopped ({} textures done by the thread, {} by the ring, {} compared, "
                "{} discarded, {} uploaded again)",
                hashes_thread_total_, hashes_ring_total_, hashes_compared_, hashes_discarded_,
                hashes_reuploads_);
  }

  // Every 10 s, if there were new textures: what the thread and the ring did. This is also where it is
  // decided whether it pays off: if for 3 reports in a row (with 16 or more jobs) what the thread took off
  // the ring is less than what the ring spent copying snapshots and waiting for it, the ring would do better
  // alone, and it switches off.
  void ReportHashes() {
    if (hashes_phase_ == kHashesNoDecide) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_hashes_ < std::chrono::seconds(10)) {
      return;
    }
    report_hashes_ = now;
    bool priority_ok = true;
    int core = -1;
    {
      std::lock_guard<std::mutex> lock(hashes_mutex_);
      priority_ok = hashes_priority_ok_;
      core = hashes_core_real_;
    }
    if (hashes_planned_report_ || hashes_thread_report_ || hashes_ring_report_ || hashes_no_room_report_) {
      static constexpr const char* kPhases[] = {"off", "OBSERVING", "APPLYING"};
      BO2ZM_REPORT_RING(
          "[native] C3 new texture hashes, last 10 s: {} planned ({} for comparison); the thread prepared "
          "{} in {:.1f} ms (hash {:.1f} ms) and the ring {} at submit in {:.1f} ms; the ring copied {:.1f} MB of "
          "snapshots in {:.1f} ms and waited for the thread {:.1f} ms in {} waits (worst {:.2f} ms); {} by the usual "
          "path without room; {} compared since the start ({} could not be); thread on core {}, priority {:#x}{}; phase "
          "{}",
          hashes_planned_report_, hashes_comparison_report_, hashes_thread_report_,
          double(ns_hashes_thread_report_) / 1e6, double(ns_hash_raw_thread_report_) / 1e6,
          hashes_ring_report_, double(ns_hashes_ring_report_) / 1e6,
          double(bytes_snapshots_report_) / 1048576.0, double(ns_snapshots_report_) / 1e6,
          double(ns_wait_hashes_report_) / 1e6, waits_hashes_report_, double(ns_wait_hashes_worst_) / 1e6,
          hashes_no_room_report_, hashes_compared_, hashes_no_compare_, core, hashes_priority_,
          priority_ok ? "" : " (the kernel did NOT accept the priority)", kPhases[std::clamp<int32_t>(hashes_phase_, 0, 2)]);
    }
    if (hashes_phase_ == kHashesApplying && hashes_thread_report_ + hashes_ring_report_ >= 16) {
      const bool compensates = ns_hashes_thread_report_ > ns_snapshots_report_ + ns_wait_hashes_report_;
      reports_no_compensate_hashes_ = compensates ? 0 : reports_no_compensate_hashes_ + 1;
      if (reports_no_compensate_hashes_ >= 3) {
        TurnOffHashes(fmt::format("not worth it: in 3 reports in a row the thread took less off the ring than the ring "
                                  "spent copying and waiting for it (the last one: thread {:.1f} ms, copies {:.1f} ms, "
                                  "wait {:.1f} ms)",
                                  double(ns_hashes_thread_report_) / 1e6, double(ns_snapshots_report_) / 1e6,
                                  double(ns_wait_hashes_report_) / 1e6));
      }
    }
    hashes_planned_report_ = 0;
    hashes_comparison_report_ = 0;
    hashes_thread_report_ = 0;
    hashes_ring_report_ = 0;
    ns_hashes_thread_report_ = 0;
    ns_hash_raw_thread_report_ = 0;
    ns_hashes_ring_report_ = 0;
    ns_snapshots_report_ = 0;
    bytes_snapshots_report_ = 0;
    ns_wait_hashes_report_ = 0;
    ns_wait_hashes_worst_ = 0;
    waits_hashes_report_ = 0;
    hashes_no_room_report_ = 0;
  }

  // --- Vertex copies on a separate thread (bo2zm_native_upload_thread) ------------------------------------------
  // One producer (the ring thread, in Draw) and one consumer (copies_thread_). The producer writes the job
  // into copies_written_ and publishes it; the thread copies up to there and publishes copies_done_.
  // With the queue full the copy is done on the ring. The destinations are distinct ranges of
  // the upload buffer, so order does not matter. The copies must be waited for before submitting the work
  // (BeforeSubmit) and before returning the read pointer to the game (WaitUploads from
  // bo2zm_native_render.cpp): after that the game may reuse its vertex buffers.
  //
  // Sleeping and waking are decided under the lock. Deciding without it whether to wake the other side,
  // Dekker-style (the ring publishes copies_written_ and reads copies_done_, the thread the other way round),
  // can lose the wake-up on both sides and hang with copies queued and nobody copying: with
  // store(..., memory_order_release) MSVC's STL emits a plain mov, and on x86 (TSO) a store may not yet be
  // visible from another core; ARM gives no guarantee either. So copies_sleeping_ and copies_waiting_ are only
  // touched under the lock, and nothing is decided without it: the wake-up arrives at the next point that takes
  // the lock (every kCopiesPerWarning copies, when the ring waits, and when the thread runs out of work). The
  // thread always signals from its no-work path, under the lock, which it goes through as soon as it finishes.
  // Only the queuing thread may wait (single producer). Otherwise it wakes the thread once and waits
  // without helping.
  //
  /*
   * The ring helps instead of waiting (bo2zm_native_upload_help).
   *
   * Why. In gameplay the ring waited 14.7 ms for the copy thread during one stutter (3,965 draws, 5 waits)
   * and 17.9 ms in the next (3,890 draws, 3 waits; the game spent 21.1 ms without room in the ring). Copying
   * what those frames needed is ~9-11 MB, ~6-7 ms at the rate the profile gives the thread (1.5 GB/s): the
   * rest was the thread ready and without a core. It ran at 0x3B, below the two game threads (0x3A), and
   * Horizon only moves a ready thread to another core when that core runs out of work: with the CPU at
   * 278 % out of 300 (the profile block for that stutter), almost never. And the ring (0x2D) waited for it
   * on a condition variable, which does not lend priority: a textbook priority inversion.
   *
   * How. On every wait with pending copies (by the queuing thread, in the applying phase):
   *   1. the ring takes and copies, in batches of kCopiesPerChunk, whatever the thread has not taken yet.
   *      If the thread is running on another core they share what is left; if it has no core, the ring
   *      does it all, without waiting for it.
   *   2. whatever is missing can only be the batch the thread is working on. The ring waits by taking
   *      copies_chunk_mutex_, which the thread holds while copying: a libnx mutex waits in the kernel
   *      (svcArbitrateLock) and Horizon lends the owner the waiter's priority, so the thread rises to 0x2D
   *      until it releases it. It is one batch.
   * The thread's own priority is not changed: it takes no core from anyone while the ring is not waiting
   * for it.
   *
   * Counters. copies_written_ (ring only), copies_taken_ (the thread or the ring, with CAS, in order)
   * and copies_done_ (how many are done, whoever did them): done <= taken <= written. Outside
   * WaitUploads only the thread takes, so it finishes in order and "done = h" still means the first h
   * are done: EnqueueCopy's space accounting still holds. Inside, the ring does not queue, and it leaves
   * with everything done.
   *
   * Same data. Each copy is done exactly once by CopyVertices with the same slot (source, destination,
   * words and byte order), and all of them before returning from WaitUploads: before submitting and before
   * returning the read pointer. Only who does it changes, and sometimes it is done earlier.
   *
   * Self-checking guard. Each copy leaves its index + 1 in copies_marks_. On each wait the ring checks the
   * marks of what was queued since the previous one and that the counters add up; a copy without a mark is
   * done right there, before submitting and before returning the read pointer, and it is a MISMATCH
   * (REXLOG_ERROR). Phases: observing (the first kWaitsCopiesObserving waits with pending copies: the
   * thread already takes batches, the ring only waits and checks), applying (help) and, after a MISMATCH
   * with help on, no help (the plain wait). A MISMATCH without help, or with the counters out of step,
   * turns the thread off for the rest of the run and the copies go back to the ring. The plain wait never
   * waits forever: after 2 s it takes the batch mutex and finishes the job. The batch mutex has no timeout
   * because inside it the thread only takes and copies.
   */
  enum CopyPhase : int32_t { kCopiesNoHelp = 0, kCopiesObserving = 1, kCopiesApplying = 2 };

  void WarnOtherThreadCopies(const char* where) {
    if (!copies_other_thread_warned_) {
      copies_other_thread_warned_ = true;
      REXLOG_ERROR("[native] C6: {} from a thread that is not the one queueing the vertex copies", where);
    }
  }

  bool EnqueueCopy(const CopyJob& job) {
    if (copies_no_thread_) {
      // Thread switched off by a MISMATCH (or it could not be created): the copy goes on the ring.
      ++copies_inf_.in_line;
      copies_inf_.bytes_in_line += uint64_t(job.words) * 4;
      return false;
    }
    if (!copies_thread_.joinable()) {
      copies_producer_ = std::this_thread::get_id();
      copies_phase_ = copies_help_requested_ ? kCopiesObserving : kCopiesNoHelp;
#if REX_PLATFORM_SWITCH
      const int ring_core = RexSwitchCurrentCore();  // asked by the ring, which creates the thread
#else
      const int ring_core = -1;
#endif
      try {
        copies_thread_ = std::thread([this, ring_core] {
          rex::thread::set_current_thread_name("BO2 vertex copies");
#if REX_PLATFORM_SWITCH
          // bo2zm_native_upload_thread_priority (see the cvar).
          const int32_t priority = REXCVAR_GET(bo2zm_native_upload_thread_priority);
          const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
          // bo2zm_native_upload_thread_core (see the cvar).
          const int32_t requested = REXCVAR_GET(bo2zm_native_upload_thread_core);
          const int core = requested == -2 ? (ring_core == 2 ? 1 : 2) : int(requested);
          const bool core_ok = core < 0 || RexSwitchSetCurrentThreadCore(core);
          REXLOG_INFO("[native] C6: vertex copy thread at priority {:#x}{}; preferred core {}{} (the ring runs on {}), "
                      "now on {}",
                      priority, priority_ok ? "" : " (NOT accepted: it keeps the one it was created with)", core,
                      core_ok ? "" : " (NOT accepted)", ring_core, RexSwitchCurrentCore());
#endif
          LoopCopies();
        });
      } catch (const std::system_error& error) {
        copies_no_thread_ = true;
        REXLOG_ERROR("[native] C6: could not create the vertex copy thread ({}): the copies go on the ring",
                     error.what());
        return false;
      }
      REXLOG_INFO("[native] C6: vertex copies on a separate thread (bo2zm_native_upload_thread); the ring copies them "
                  "itself when waiting for them (bo2zm_native_upload_help) = {}",
                  copies_help_requested_
                      ? fmt::format("YES, in chunks of {}; OBSERVING phase during the first {} waits with pending "
                                    "copies",
                                    kCopiesPerChunk, kWaitsCopiesObserving)
                      : std::string("no, waits for the thread as before"));
    } else if (std::this_thread::get_id() != copies_producer_) {
      WarnOtherThreadCopies("EnqueueCopy");
    }
    const size_t written = copies_written_.load(std::memory_order_relaxed);
    if (written - copies_done_.load(std::memory_order_acquire) >= copies_.size()) {
      ++copies_in_line_;
      ++copies_inf_.in_line;
      copies_inf_.bytes_in_line += uint64_t(job.words) * 4;
      return false;
    }
    copies_[written & (copies_.size() - 1)] = job;
    copies_written_.store(written + 1);
    ++copies_inf_.queued;
    copies_inf_.bytes_queued += uint64_t(job.words) * 4;
    // Wake-up every kCopiesPerWarning copies: waking the thread as soon as it slept woke it on almost every
    // copy, and every wake-up goes through the kernel (1.4 % of the ring and almost all of the copy thread's
    // CPU). Whatever is left without a wake-up is collected by WaitUploads.
    if (((written + 1) & (kCopiesPerWarning - 1)) == 0) {
      WakeCopies();
    }
    return true;
  }

  void WakeCopies() {
    bool warn;
    {
      std::lock_guard<std::mutex> lock(copies_mutex_);
      warn = copies_sleeping_;
    }
    if (warn) {
      copies_cv_.notify_one();
    }
  }

  // Fence measurement only (bo2zm_native_render.cpp, AnnotateFenceCopies).
  size_t CopiesPending() const override {
    if (!copies_thread_.joinable()) {
      return 0;
    }
    const size_t written = copies_written_.load(std::memory_order_relaxed);
    const size_t done = copies_done_.load(std::memory_order_relaxed);
    return written > done ? written - done : 0;
  }

  void WaitUploads() override {
    if (!copies_thread_.joinable()) {
      return;
    }
    const bool producer = std::this_thread::get_id() == copies_producer_;
    if (!producer) {
      WarnOtherThreadCopies("WaitUploads");
    }
    const size_t goal = copies_written_.load(std::memory_order_relaxed);
    if (copies_done_.load(std::memory_order_acquire) == goal) {
      if (producer) {
        CheckCopies(goal);  // what the thread did since the last wait
      }
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    // What the thread had not taken yet on arrival (while observing, what the ring would have copied).
    copies_inf_.no_take += goal - std::min(goal, copies_taken_.load(std::memory_order_acquire));
    uint64_t ns_helping = 0;
    uint64_t helped = 0;
    if (producer && copies_phase_ == kCopiesApplying) {
      // 1. What the thread has not taken yet, the ring copies in batches: if the thread is running, they
      //    share it.
      uint64_t bytes = 0;
      while (TakeAndCopy(true, helped, bytes)) {
      }
      const auto after_help = std::chrono::steady_clock::now();
      ns_helping = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_help - before).count());
      copies_helped_ += helped;
      copies_bytes_helped_ += bytes;
      copies_ns_helping_ += ns_helping;
      copies_inf_.helped += helped;
      copies_inf_.bytes_helped += bytes;
      copies_inf_.ns_helping += ns_helping;
      if (helped) {
        ++copies_inf_.waits_with_help;
      }
      // 2. What is missing is the batch the thread is working on: wait on its mutex, which lends the thread
      //    the ring's priority until it releases it.
      if (copies_done_.load(std::memory_order_acquire) != goal) {
        {
          std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
          if (copies_done_.load(std::memory_order_acquire) != goal) {
            RepairCopies(goal, "with the chunk lock held the thread has nothing in hand and copies are still missing");
          }
        }
        const uint64_t ns_chunk = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - after_help)
                                               .count());
        ++copies_inf_.waits_chunk;
        copies_inf_.ns_chunk += ns_chunk;
        copies_inf_.ns_chunk_worst = std::max(copies_inf_.ns_chunk_worst, ns_chunk);
      }
    } else {
      WaitCopiesAsAlways(goal);
    }
    if (producer) {
      CheckCopies(goal);
      if (copies_phase_ == kCopiesObserving && ++copies_waits_observing_ >= kWaitsCopiesObserving) {
        copies_phase_ = kCopiesApplying;
        REXLOG_INFO("[native] C6: vertex copies: {} waits with pending copies in the OBSERVING phase and {} copies "
                    "checked, all with their mark and the counters adding up. Moves to the APPLYING phase: the ring "
                    "copies what the thread has not taken and waits for the chunk in progress with its lock",
                    copies_waits_observing_, copies_checked_);
      }
    }
    ++waits_copies_;
    const uint64_t ns_total = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before)
                                           .count());
    // The waiting time counts only the wait; what the ring copies is counted separately (as helping).
    const uint64_t ns_wait = ns_total > ns_helping ? ns_total - ns_helping : 0;
    ns_waiting_copies_ += ns_wait;
    ++copies_inf_.waits;
    copies_inf_.ns_wait += ns_wait;
    copies_inf_.ns_wait_worst = std::max(copies_inf_.ns_wait_worst, ns_wait);
    // And to the "[hitch] ring" line (bo2zm_hitch_waits.h). This is only reached when the ring really
    // waits, a few times per frame: the fetch_add calls do not show.
    bo2zm::waits::g_ns_waiting_copies.fetch_add(ns_wait, std::memory_order_relaxed);
    bo2zm::waits::g_waits_copies.fetch_add(1, std::memory_order_relaxed);
    bo2zm::waits::g_ns_helping_copies.fetch_add(ns_helping, std::memory_order_relaxed);
    bo2zm::waits::g_copies_helped.fetch_add(helped, std::memory_order_relaxed);
  }

  // The plain wait (observing and no-help phases, or when another thread waits): the ring sleeps until the
  // thread finishes. After 2 s it stops waiting: it takes the batch mutex, which lends its priority to the
  // thread if it is copying, and if copies are still missing with the thread out of its loop, the ring does
  // them and switches the thread off.
  void WaitCopiesAsAlways(size_t goal) {
    bool a_time = false;
    {
      std::unique_lock<std::mutex> lock(copies_mutex_);
      if (copies_sleeping_) {
        copies_cv_.notify_one();  // a wake-up EnqueueCopy skipped (it only wakes every kCopiesPerWarning copies)
      }
      copies_waiting_ = true;
      a_time = copies_done_cv_.wait_for(lock, std::chrono::seconds(2), [this, goal] {
        return copies_done_.load(std::memory_order_acquire) == goal;
      });
      copies_waiting_ = false;
    }
    ++copies_inf_.waits_whole;
    if (a_time) {
      return;
    }
    // Should never happen: diagnostic for a hang once seen in the menus.
    REXLOG_ERROR("[native] C6: the ring thread has been waiting 2 s for the vertex copies: target {}, done {}, taken "
                 "{}, written {}, copied by the thread {}",
                 goal, copies_done_.load(std::memory_order_acquire),
                 copies_taken_.load(std::memory_order_acquire), copies_written_.load(std::memory_order_acquire),
                 copies_progress_.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
    if (copies_done_.load(std::memory_order_acquire) != goal) {
      RepairCopies(goal, "2 s waiting for the thread and, with the thread out of its loop, copies are still missing");
    }
  }

  // Takes the next untaken batch (up to kCopiesPerChunk copies, in order) and copies it. Used by the
  // thread, holding copies_chunk_mutex_, and by the ring when helping. false = nothing was left to take.
  bool TakeAndCopy(bool ring, uint64_t& copies, uint64_t& bytes) {
    const size_t written = copies_written_.load(std::memory_order_acquire);
    size_t taken = copies_taken_.load(std::memory_order_acquire);
    size_t n = 0;
    do {
      if (taken >= written) {
        return false;
      }
      n = std::min<size_t>(written - taken, kCopiesPerChunk);
    } while (!copies_taken_.compare_exchange_weak(taken, taken + n, std::memory_order_acq_rel,
                                                    std::memory_order_acquire));
    const size_t mask = copies_.size() - 1;
    for (size_t i = taken; i < taken + n; ++i) {
      const CopyJob& job = copies_[i & mask];
      CopyVertices(job);
      bytes += uint64_t(job.words) * 4;
      copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
      if (!ring) {
        copies_progress_.store(i + 1, std::memory_order_relaxed);  // for the WaitUploads diagnostic
      }
    }
    copies += n;
    copies_done_.fetch_add(n, std::memory_order_acq_rel);  // publishes the copies and their marks
    return true;
  }

  bool HasCopiesNoTake() const {
    return copies_taken_.load(std::memory_order_acquire) != copies_written_.load(std::memory_order_acquire);
  }

  // The guard, on each wait and with everything finished (the ring does not queue while waiting and the
  // thread has nothing half taken): what was queued since the previous one has its mark and the counters
  // add up. Ring only.
  void CheckCopies(size_t goal) {
    if (goal == copies_verified_ || copies_no_thread_) {
      return;
    }
    const size_t mask = copies_.size() - 1;
    const size_t since =
        std::max(copies_verified_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
    size_t no_mark = 0;
    size_t first = 0;
    for (size_t i = since; i < goal; ++i) {
      if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        if (!no_mark) {
          first = i;
        }
        ++no_mark;
      }
    }
    copies_checked_ += goal - since;
    copies_inf_.checked += goal - since;
    const size_t taken = copies_taken_.load(std::memory_order_acquire);
    const size_t done = copies_done_.load(std::memory_order_acquire);
    if (!no_mark && taken == goal && done == goal) {
      copies_verified_ = goal;
      return;
    }
    const std::string reason = no_mark ? fmt::format("{} of {} copies without their mark (the first, number "
                                                     "{})", no_mark,
                                                       goal - since, first)
                                         : std::string("all with their mark, but the counters do not add up");
    std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);  // the thread, out of its copy loop
    if (copies_phase_ == kCopiesApplying && taken == goal && done == goal) {
      // With the counters right, the suspect is the help path: redo whatever is missing and go back to the
      // plain wait.
      const size_t redone = RedoNoMark(goal);
      copies_verified_ = goal;
      ++copies_mismatches_;
      copies_phase_ = kCopiesNoHelp;
      REXLOG_ERROR("[native] C6: vertex copies: MISMATCH: {} ({} queued, counters adding up; {} redone on the ring "
                   "before submitting). Ring help OFF for the rest of the session: waits for the thread as before",
                   reason, goal, redone);
      return;
    }
    RepairCopies(goal, reason);
  }

  // Copies on the ring whatever was queued up to `goal` (since the last check, and at most one lap of
  // the queue) that lacks its mark, and marks it. With copies_chunk_mutex_ held. Returns how many.
  size_t RedoNoMark(size_t goal) {
    const size_t mask = copies_.size() - 1;
    const size_t since =
        std::max(copies_verified_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
    size_t redone = 0;
    for (size_t i = since; i < goal; ++i) {
      if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        CopyVertices(copies_[i & mask]);
        copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
        ++redone;
      }
    }
    return redone;
  }

  // Should never happen. With copies_chunk_mutex_ held, i.e. with the thread out of its copy loop and
  // nothing half taken: the ring copies whatever is missing up to `goal` (before submitting and before
  // returning the read pointer), the counters are left consistent and the thread is switched off for the
  // rest of the run.
  void RepairCopies(size_t goal, const std::string& reason) {
    const size_t taken = copies_taken_.load(std::memory_order_acquire);
    const size_t done = copies_done_.load(std::memory_order_acquire);
    const size_t redone = RedoNoMark(goal);
    copies_taken_.store(goal, std::memory_order_release);
    copies_done_.store(goal, std::memory_order_release);
    copies_verified_ = goal;
    ++copies_mismatches_;
    copies_no_thread_ = true;
    REXLOG_ERROR("[native] C6: vertex copies: MISMATCH: {} (queued {}, taken {}, done {}; {} redone on the ring before "
                 "submitting). Copy thread OFF for the rest of the session: the copies go on the ring, as before build "
                 "90",
                 reason, goal, taken, done, redone);
  }

  const char* NamePhaseCopies() const {
    if (copies_no_thread_) {
      return "NO THREAD (copies on the ring)";
    }
    return copies_phase_ == kCopiesApplying ? "APPLYING" : copies_phase_ == kCopiesObserving ? "OBSERVING" : "NO HELP";
  }

  // Every 10 s, if there were copies: who did them and how long the ring really waited. Ring only.
  void ReportCopies() {
    if (!copies_thread_.joinable() && !copies_no_thread_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - copies_report_ < std::chrono::seconds(10)) {
      return;
    }
    copies_report_ = now;
    const uint64_t thread_n = copies_thread_n_.load(std::memory_order_relaxed);
    const uint64_t thread_bytes = copies_thread_bytes_.load(std::memory_order_relaxed);
    const uint64_t d_thread_n = thread_n - copies_thread_n_previous_;
    const uint64_t d_thread_bytes = thread_bytes - copies_thread_bytes_previous_;
    copies_thread_n_previous_ = thread_n;
    copies_thread_bytes_previous_ = thread_bytes;
    const CopyReportFigures& c = copies_inf_;
    if (c.queued || c.in_line || c.waits) {
      BO2ZM_REPORT_RING(
          "[native] C6 vertex copies, last 10 s: {} queued ({:.1f} MB) and {} on the ring without going "
          "through the queue ({:.1f} MB); the thread copied {} ({:.1f} MB) and the ring helped with {} ({:.1f} MB, "
          "{:.1f} ms) in {} of {} waits with pending copies ({} not taken on arrival); waited for the thread's chunk {} "
          "times ({:.2f} ms, worst {:.2f} ms) and the whole thing {} times; pure waiting {:.1f} ms (worst {:.2f} ms); "
          "{} copies checked, {} mismatches; phase {}",
          c.queued, double(c.bytes_queued) / 1048576.0, c.in_line, double(c.bytes_in_line) / 1048576.0,
          d_thread_n, double(d_thread_bytes) / 1048576.0, c.helped, double(c.bytes_helped) / 1048576.0,
          double(c.ns_helping) / 1e6, c.waits_with_help, c.waits, c.no_take, c.waits_chunk,
          double(c.ns_chunk) / 1e6, double(c.ns_chunk_worst) / 1e6, c.waits_whole, double(c.ns_wait) / 1e6,
          double(c.ns_wait_worst) / 1e6, c.checked, copies_mismatches_, NamePhaseCopies());
    }
    copies_inf_ = CopyReportFigures{};
  }

  // The per-fetch-constant sampler table, every 10 s: hits, and misses by cause.
  void ReportCacheFetch() {
    const auto now = std::chrono::steady_clock::now();
    if (now - fetch_report_ < std::chrono::seconds(10)) {
      return;
    }
    fetch_report_ = now;
    {  // DedupeIndices
      const uint64_t figures[4] = {dedupe_indices_hits_, dedupe_indices_converted_, bytes_indices_saved_,
                                   dedupe_indices_checks_};
      uint64_t di[4];
      for (int i = 0; i < 4; ++i) {
        di[i] = figures[i] - dedupe_indices_report_[i];
        dedupe_indices_report_[i] = figures[i];
      }
      BO2ZM_REPORT_RING("[native] C6 upload dedupe across cache flush waits ({}): {} vertex reuses checked by hash, "
                        "{} with different bytes",
                        g_syncs_on_flush_waits.load(std::memory_order_relaxed) ? "OFF: forgets at every wait" : "on",
                        dedupe_checked_, dedupe_.discrepancies());
      BO2ZM_REPORT_RING("[native] C6 index dedupe (bo2zm_native_dedupe_indices {}), last 10 s: {} index ranges reused "
                        "without a scan ({} of them converted again after a submit), {:.1f} MB of indices not "
                        "uploaded; {} reuses checked against a fresh scan ({} in total)",
                        dedupe_indices_on_ ? "on" : (dedupe_indices_off_ ? "OFF after a MISMATCH" : "off"), di[0],
                        di[1], double(di[2]) / 1048576.0, di[3], dedupe_indices_checks_);
    }
    const std::array<uint64_t, 9> now_figures{samplers_cache_fetch_, fetch_misses_collision_, fetch_misses_empty_,
                                               fetch_misses_generation_, fetch_misses_expired_, samplers_cache_,
                                               reuse_hits_, reuse_checks_ + reuse_sampled_ / 1024, reuse_by_value_};
    std::array<uint64_t, 9> d{};
    for (size_t i = 0; i < d.size(); ++i) {
      d[i] = now_figures[i] - fetch_report_previous_[i];
    }
    fetch_report_previous_ = now_figures;
    const uint64_t misses = d[1] + d[2] + d[3] + d[4];
    if (d[0] + misses == 0) {
      return;
    }
    const std::array<uint64_t, 3> invalidations{invalidations_global_, invalidations_retired_,
                                                 invalidations_targeted_};
    std::array<uint64_t, 3> di{};
    for (size_t i = 0; i < di.size(); ++i) {
      di[i] = invalidations[i] - invalidations_previous_[i];
    }
    invalidations_previous_ = invalidations;
    BO2ZM_REPORT_RING(
        "[native] C6 sampler cache by fetch ({} cells), last 10 s: {} register hits, {} table lookups with "
        "{} hits and {} misses ({:.1f} %): {} collisions with another fetch in its cell, {} in an empty cell, {} by "
        "generation and {} expired (texture due for a check); invalidations: {} of the whole cache, {} by retired "
        "images, {} of one address only; texture words reused from the previous draw {} times ({}, ~{} checked; {} by "
        "value after a fetch write)",
        cache_fetch_.size(), d[5], d[0] + misses, d[0], misses,
        100.0 * double(misses) / double(std::max<uint64_t>(d[0] + misses, 1)), d[1], d[2], d[3], d[4], di[0], di[1],
        di[2], d[6], reuse_textures_on_ ? "on" : "off", d[7], d[8]);
  }

  void StopCopies() {
    if (!copies_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(copies_mutex_);
      copies_stop_ = true;
    }
    copies_cv_.notify_one();
    copies_thread_.join();
    REXLOG_INFO("[native] C6: vertex copy thread stopped ({} copies on the ring because the queue was full, {} ring "
                "waits, {:.1f} ms waiting; the ring copied {} while waiting ({:.1f} MB, {:.1f} ms), "
                "{} checked, {} mismatches, phase {})",
                copies_in_line_, waits_copies_, double(ns_waiting_copies_) / 1e6, copies_helped_,
                double(copies_bytes_helped_) / 1048576.0, double(copies_ns_helping_) / 1e6, copies_checked_,
                copies_mismatches_, NamePhaseCopies());
  }

  void LoopCopies() {
    for (;;) {
      if (HasCopiesNoTake()) {
        // Holding copies_chunk_mutex_ from the first batch until there is nothing left to take, and never
        // waiting on anything inside. If the ring waits for the current batch, it waits on this mutex and the
        // thread runs at the ring's priority until it releases it.
        uint64_t copies = 0;
        uint64_t bytes = 0;
        {
          std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
          while (TakeAndCopy(false, copies, bytes)) {
          }
        }
        copies_thread_n_.fetch_add(copies, std::memory_order_relaxed);
        copies_thread_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        continue;  // look again: if nothing is left, sleep through the path below
      }
      // Nothing to take (the ring may have taken the last ones). Under the lock: wake the ring if it waits,
      // and sleep until there is more. If the read of copies_written_ was stale, the ring signals at its next
      // point that takes the lock.
      std::unique_lock<std::mutex> lock(copies_mutex_);
      if (copies_waiting_) {
        copies_done_cv_.notify_one();
      }
      copies_sleeping_ = true;
      copies_cv_.wait(lock, [this] { return copies_stop_ || HasCopiesNoTake(); });
      copies_sleeping_ = false;
      /*
       * The thread exits only when asked to stop. Being woken with no work does not mean "stop": the ring
       * takes untaken copies when it waits for them (TakeAndCopy, without copies_mutex_), so the thread can
       * wake because there were copies and find that the ring already took them. Exiting there would leave
       * the ring doing every copy for the rest of the run (3-4 ms more per frame). So it looks again.
       */
      if (copies_stop_ && !HasCopiesNoTake()) {
        return;  // stop, nothing pending
      }
    }
  }

  // bo2zm_visual_treatment. If the draw is the final composite and the filter is not the original one, uploads its
  // own copy of the constants with the color curve changed, and leaves the cache empty so the next draw uploads the
  // game's. If there is no room in the upload buffer (Draw does not count this copy), the frame keeps the original
  // filter.
  // It is out of line and Draw always calls it: that way Draw gains no branch and its control flow is still the
  // one in the PGO profile. With a branch inside, GCC would drop the whole profile of the ring's hottest function.
  [[gnu::noinline]] void CopyCompositeTreated(const ShaderInput* ps, const uint32_t* r,
                                                 uint32_t bytes_ps) noexcept {
    if (!ps || !ps->shader || ps->shader->hash != kHashComposite) {
      return;
    }
    const int mode = bo2zm::settings::TreatmentVisual();
    // One line per mode change, on the first draw of the composite: the values the game sets.
    static std::atomic<int> mode_annotated{-1};
    if (mode_annotated.exchange(mode, std::memory_order_relaxed) != mode) {
      const float* k = reinterpret_cast<const float*>(r + kRegConstantsPs);
      REXLOG_INFO("[native] color filter {}: composite with {} bytes of constants. The game sets Coeffs0 ({:.3f}, "
                  "{:.3f}, {:.3f}, {:.3f}), Coeffs1 ({:.3f}, {:.3f}, {:.3f}, {:.3f}), Desaturation {:.3f} and vignette "
                  "{:.3f}",
                  mode, bytes_ps, k[24], k[25], k[26], k[27], k[28], k[29], k[30], k[31], k[8], k[4]);
    }
    if (bytes_ps < kBytesTreatment) {
      return;
    }
    VkDeviceSize offset = 0;
    if (mode == 0 || !Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps,
                               use_ubo_ ? alignment_ubo_ : 16, offset)) {
      return;
    }
    std::memcpy(upload_data_ + offset, r + kRegConstantsPs, bytes_ps);
    ApplyTreatmentVisual(reinterpret_cast<float*>(upload_data_ + offset), mode);
    constants_ps_offset_ = offset;
    constants_ps_generation_ = UINT64_MAX;
  }

  bool Reserve(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize& offset) {
    const VkDeviceSize start = (upload_used_ + alignment - 1) & ~(alignment - 1);
    if (start + bytes > kSizeUpload) {
      offset = 0;
      return false;  // cannot happen: Draw checks the space first
    }
    offset = start;
    upload_used_ = start + bytes;
    megs_bytes_ += bytes;
    return true;
  }

  // Like Reserve, but the offset is a multiple of `multiple` (the stride: a multiple of 4, not always a
  // power of 2). Draw has already asked for space including that extra padding (gap_base_zero).
  bool ReserveMultiple(VkDeviceSize bytes, VkDeviceSize multiple, VkDeviceSize& offset) {
    const VkDeviceSize start = (upload_used_ + multiple - 1) / multiple * multiple;
    if (start + bytes > kSizeUpload) {
      offset = 0;
      return false;  // cannot happen: Draw checks the space first
    }
    offset = start;
    upload_used_ = start + bytes;
    megs_bytes_ += bytes;
    return true;
  }

  // The pipelines file (cache/bo2zm_native_pipelines.bin): the Vulkan cache and the prewarm list.
  static std::filesystem::path PathFilePipelines() {
    return bo2zm::native::pso::CacheFolder() / kFilePipelines;
  }

  static void ReadFileWhole(const std::filesystem::path& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff bytes = file ? std::streamoff(file.tellg()) : 0;
    if (bytes > 0 && bytes < (std::streamoff(256) << 20)) {
      data.resize(size_t(bytes));
      file.seekg(0);
      if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(bytes))) {
        data.clear();
      }
    }
  }

  // Reads the pipelines file and splits it into the cache and the list. If it does not exist yet, the two
  // older files (next to the NRO), which are deleted when the new one is written.
  void ReadFilePipelines(std::vector<uint8_t>& cache, std::vector<uint8_t>& list) {
    std::vector<uint8_t> file_bytes;
    ReadFileWhole(PathFilePipelines(), file_bytes);
    if (!file_bytes.empty()) {
      uint32_t magic = 0;
      uint32_t version = 0;
      uint64_t bytes_list = 0;
      uint64_t bytes_cache = 0;
      if (file_bytes.size() >= kHeaderFilePipelines) {
        std::memcpy(&magic, file_bytes.data(), 4);
        std::memcpy(&version, file_bytes.data() + 4, 4);
        std::memcpy(&bytes_list, file_bytes.data() + 8, 8);
        std::memcpy(&bytes_cache, file_bytes.data() + 16, 8);
      }
      if (magic != kMagicFilePipelines || version != kVersionFilePipelines ||
          bytes_list > file_bytes.size() || bytes_cache > file_bytes.size() ||
          kHeaderFilePipelines + bytes_list + bytes_cache != file_bytes.size()) {
        REXLOG_WARN("[native] C6: pipeline file from another version or damaged: starting from scratch");
        return;
      }
      const auto start = file_bytes.begin() + kHeaderFilePipelines;
      list.assign(start, start + std::ptrdiff_t(bytes_list));
      cache.assign(start + std::ptrdiff_t(bytes_list), file_bytes.end());
      return;
    }
    const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
    ReadFileWhole(folder / kFileCacheOld, cache);
    ReadFileWhole(folder / kFileListOld, list);
    files_old_ = !cache.empty() || !list.empty();
  }

  // On-disk pipeline cache (derived from the game's shaders: not distributed). If the driver rejects it
  // (another driver or corrupt data), it starts from scratch.
  void LoadCachePipelines() {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    const auto create = reinterpret_cast<FnCreatePipelineCache>(
        ifn.vkGetDeviceProcAddr(device_, "vkCreatePipelineCache"));
    data_cache_ = reinterpret_cast<FnPipelineCacheData>(
        ifn.vkGetDeviceProcAddr(device_, "vkGetPipelineCacheData"));
    destroy_cache_ = reinterpret_cast<FnDestroyPipelineCache>(
        ifn.vkGetDeviceProcAddr(device_, "vkDestroyPipelineCache"));
    if (!create || !data_cache_ || !destroy_cache_) {
      REXLOG_WARN("[native] C6: no pipeline cache (the driver does not provide its functions)");
      return;
    }
    const std::filesystem::path path = PathFilePipelines();
    std::vector<uint8_t> data;
    ReadFilePipelines(data, list_read_);
    if (REXCVAR_GET(bo2zm_native_test_fresh_pipelines)) {
      REXLOG_INFO("[native] C6: bo2zm_native_test_fresh_pipelines: empty pipeline cache and prewarm list ({} KB and {} "
                  "KB on disk ignored, and not saved over)",
                  data.size() >> 10, list_read_.size() >> 10);
      data.clear();
      list_read_.clear();
    } else if (REXCVAR_GET(bo2zm_native_test_cold_pipelines)) {
      REXLOG_INFO("[native] C6: bo2zm_native_test_cold_pipelines: empty pipeline cache ({} KB on disk ignored), the "
                  "list kept",
                  data.size() >> 10);
      data.clear();
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      data.clear();
      if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
        cache_pipelines_ = VK_NULL_HANDLE;
      }
    }
    bytes_cache_saved_ = data.size();
    cache_saved_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[native] C6: pipeline cache {} ({} KB read from {}{})",
                cache_pipelines_ != VK_NULL_HANDLE ? "on" : "not available",
                data.size() >> 10, path.string(), files_old_ ? ", from the two earlier files" : "");
    // The writer thread always saves both parts; it starts from what is on disk.
    written_cache_ = std::move(data);
    if (files_old_) {
      // they came from the two older files: moved to the new one on the first save even if nothing changed
      bytes_cache_saved_ = 0;
      pipelines_no_save_ = 1;
      list_no_save_ = 1;
    }
    LoadListPipelines();  // the prewarm list
  }

  // The ring only asks; the writer thread takes the cache's data from the driver itself (done on the ring,
  // that would be ~43 MB copied twice for every save). in_match: written only if the cache grew, otherwise the list
  // waits for the next save outside a match (writer_list_waiting_).
  void SaveCachePipelines(bool in_match = false) {
    cache_saved_ = std::chrono::steady_clock::now();
    if (REXCVAR_GET(bo2zm_native_test_fresh_pipelines)) {
      return;  // the real file stays as it was
    }
    // The prewarm list is saved even if the cache does not grow (a new pipeline whose shaders were already
    // in it does not make it grow). The cache, as usual: only if it has new entries.
    std::vector<uint8_t> list;
    if (list_no_save_) {
      list_no_save_ = 0;
      list = SerializeListPipelines();
    }
    const bool want_cache = pipelines_no_save_ != 0 && cache_pipelines_ != VK_NULL_HANDLE;
    pipelines_no_save_ = 0;
    pipelines_seen_ = 0;
    // A list held back in a match is written now, outside one.
    const bool flush = !in_match && writer_list_waiting_.exchange(false, std::memory_order_relaxed);
    // The file is written on its own thread: on the Switch, writing about 2 MB to the SD card from the ring
    // thread stalled it on every save. If the previous one has not been written yet, this one replaces it.
    std::lock_guard<std::mutex> lock(writer_mutex_);
    if (!want_cache && list.empty() && !writer_resave_ && !flush) {
      return;
    }
    StartWriterCache();
    if (!list.empty()) {
      writer_list_ = std::move(list);
    }
    writer_want_cache_ = writer_want_cache_ || want_cache;
    writer_flush_ = writer_flush_ || flush;
    writer_in_match_ = in_match;
    writer_verified_ = ListVerified();  // for the stamp file (bo2zm_native_precompile.h)
    writer_pending_ = true;
    writer_warning_.notify_one();
  }

  // Whether every pipeline the list holds now is in the driver's cache: the file's were vouched for at
  // start (the stamp file) or walked by the precompile or the prewarm in this run, and the ring's own are created.
  bool ListVerified() const {
    if (bo2zm::native::precompile::KnownWarm() || precompiled_) {
      return true;
    }
    if (prewarm_stop_.load(std::memory_order_relaxed)) {
      return false;  // stopped (MISMATCH, or the renderer closing) before the end of the list
    }
    if (list_file_.empty()) {
      return prewarm_decided_;
    }
    return prewarm_finished_.load(std::memory_order_acquire) &&
           prewarm_until_.load(std::memory_order_acquire) >= list_file_.size();
  }

  // The stamp file for the pipelines file as it is on disk, which nothing new is waiting to be added to
  // (the precompile found the whole list in the cache, or the prewarm finished without compiling).
  void RequestVouch() {
    vouch_requested_ = true;
    if (REXCVAR_GET(bo2zm_native_test_fresh_pipelines) || cache_pipelines_ == VK_NULL_HANDLE) {
      return;
    }
    std::lock_guard<std::mutex> lock(writer_mutex_);
    StartWriterCache();
    writer_vouch_ = true;
    writer_pending_ = true;
    writer_warning_.notify_one();
  }

  // With writer_mutex_.
  void StartWriterCache() {
    if (!writer_cache_.joinable()) {
      writer_cache_ = std::thread([this] {
        rex::thread::set_current_thread_name("BO2 pipeline cache");
        WriterCacheMain();
      });
    }
  }

  // Writer thread. The cache's data if it changed size since the last one written (without new entries
  // it is not rewritten), or always for a test resave; empty otherwise.
  std::vector<uint8_t> TakeCacheData(bool always) {
    size_t bytes = 0;
    if (cache_pipelines_ == VK_NULL_HANDLE || data_cache_(device_, cache_pipelines_, &bytes, nullptr) != VK_SUCCESS ||
        !bytes || (bytes == bytes_cache_saved_ && !always)) {
      return {};
    }
    std::vector<uint8_t> data(bytes);
    const VkResult result = data_cache_(device_, cache_pipelines_, &bytes, data.data());
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !bytes) {
      return {};
    }
    data.resize(bytes);
    bytes_cache_saved_ = bytes;
    return data;
  }

  // Pipeline cache writer thread. Sleeping or continuing is decided with the lock held.
  void WriterCacheMain() {
    std::vector<uint8_t> list;  // the prewarm one
    std::vector<uint8_t> export_list;
    for (;;) {
      bool want_cache = false, in_match = false, flush = false, verified = false, vouch = false;
      int resave = 0;
      {
        std::unique_lock<std::mutex> lock(writer_mutex_);
        writer_warning_.wait(lock, [this] { return writer_pending_ || writer_stop_; });
        if (!writer_pending_) {
          return;  // stop, nothing pending
        }
        list = std::move(writer_list_);
        writer_list_ = {};
        want_cache = writer_want_cache_;
        writer_want_cache_ = false;
        flush = writer_flush_;
        writer_flush_ = false;
        in_match = writer_in_match_;
        resave = writer_resave_;
        writer_resave_ = 0;
        export_list = std::move(writer_export_);
        writer_export_ = {};
        verified = writer_verified_;
        vouch = writer_vouch_;
        writer_vouch_ = false;
        writer_pending_ = false;
      }
      if (!export_list.empty()) {
        const std::filesystem::path path =
            rex::filesystem::GetExecutableFolder() / "logs" / "bo2zm_prewarm_list.bin";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(export_list.data()), std::streamsize(export_list.size()));
        REXLOG_INFO("[native] C6 prewarm list written to {} ({} pipelines; bo2zm_native_test_export_list)",
                    path.string(), (export_list.size() - kHeaderList) / sizeof(PipelineRecord));
        export_list.clear();
      }
      const auto before = std::chrono::steady_clock::now();
      std::vector<uint8_t> data;
      if (want_cache || resave) {
        data = TakeCacheData(resave != 0);
      }
      const double ms_taken =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
      const bool grew = !data.empty();
      // A single file with both parts. Only one may arrive (the cache did not grow, or the list did not
      // change): the other is the last one written.
      if (grew) {
        written_cache_ = std::move(data);
      }
      const bool new_list = !list.empty();
      if (new_list) {
        written_list_ = std::move(list);
      }
      if (!grew && !new_list && !flush && !resave) {
        if (vouch) {
          bo2zm::native::precompile::Vouch(PathFilePipelines());  // the file on disk, unchanged
        }
        continue;  // nothing new
      }
      if (in_match && !grew && !resave) {
        // The cache did not grow: the list alone does not justify ~43 MB on the SD card in a match.
        writer_list_waiting_.store(true, std::memory_order_relaxed);
        continue;
      }
      // The stamp file goes before the file it would no longer describe, and comes back after it when
      // every listed pipeline is in it.
      bo2zm::native::precompile::Withdraw(PathFilePipelines());
      if (WriteFilePipelines(resave != 1, ms_taken) && (verified || vouch)) {
        bo2zm::native::precompile::Vouch(PathFilePipelines());
      }
    }
  }

  // cache/bo2zm_native_pipelines.bin with the list and the cache, written to a temporary file that is then
  // renamed. Writer thread only. The first time it is written, the two older files are deleted if present.
  // in_slices writes 256 KB at a time with a 20 ms pause after each (unless the renderer is stopping),
  // so the game's own reads (the shader library, textures, sounds) wait for one slice at most, not the whole file.
  bool WriteFilePipelines(bool in_slices, double ms_taken) {
    const auto before = std::chrono::steady_clock::now();
    const std::filesystem::path path = PathFilePipelines();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path temporal = path;
    temporal += ".tmp";
    {
      const uint32_t magic = kMagicFilePipelines;
      const uint32_t version = kVersionFilePipelines;
      const uint64_t bytes_list = written_list_.size();
      const uint64_t bytes_cache = written_cache_.size();
      std::ofstream file(temporal, std::ios::binary | std::ios::trunc);
      if (!file || !file.write(reinterpret_cast<const char*>(&magic), 4) ||
          !file.write(reinterpret_cast<const char*>(&version), 4) ||
          !file.write(reinterpret_cast<const char*>(&bytes_list), 8) ||
          !file.write(reinterpret_cast<const char*>(&bytes_cache), 8) ||
          !WriteInSlices(file, written_list_.data(), bytes_list, in_slices) ||
          !WriteInSlices(file, written_cache_.data(), bytes_cache, in_slices)) {
        return false;
      }
    }
    error.clear();
    std::filesystem::rename(temporal, path, error);
    if (error) {
      std::filesystem::remove(path, error);
      std::filesystem::rename(temporal, path, error);
    }
    if (!error && files_old_) {
      const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
      std::error_code no_importance;
      std::filesystem::remove(folder / kFileCacheOld, no_importance);
      std::filesystem::remove(folder / kFileListOld, no_importance);
      files_old_ = false;
    }
    if (saved_cache_++ < 8) {
      REXLOG_INFO("[native] C6: pipelines saved ({} KB of cache and {} in the prewarm list, in {} ms {}, from "
                  "their thread{}; the cache taken from the driver there in {:.1f} ms)",
                  written_cache_.size() >> 10,
                  written_list_.size() >= kHeaderList
                      ? (written_list_.size() - kHeaderList) / sizeof(PipelineRecord)
                      : 0,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before)
                      .count(),
                  in_slices ? "in slices" : "at once", error ? ", could not rename" : "", ms_taken);
    }
    return !error;
  }

  bool WriteInSlices(std::ofstream& file, const uint8_t* data, size_t bytes, bool in_slices) {
    constexpr size_t kSlice = 256 * 1024;
    for (size_t at = 0; at < bytes;) {
      const size_t n = in_slices ? std::min(kSlice, bytes - at) : bytes - at;
      if (!file.write(reinterpret_cast<const char*>(data + at), std::streamsize(n))) {
        return false;
      }
      at += n;
      if (at < bytes && !writer_hurry_.load(std::memory_order_relaxed)) {
        file.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    return true;
  }

  void StopWriterCache() {
    writer_hurry_.store(true, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(writer_mutex_);
      writer_stop_ = true;
      writer_warning_.notify_one();
    }
    if (writer_cache_.joinable()) {
      writer_cache_.join();
    }
  }

  // One upload buffer per work slot of the render target code: the previous frame's may still be in use
  // on the GPU while the other is filled.
  bool CreateUpload() {
    for (UploadBuffer& s : uploads_) {
      const bool created = CreateBufferUpload();
      s = {upload_, upload_memory_, upload_size_real_, upload_data_, upload_address_};
      upload_ = VK_NULL_HANDLE;
      upload_memory_ = VK_NULL_HANDLE;
      upload_data_ = nullptr;
      if (!created) {
        return false;
      }
    }
    const auto& types = vulkan_device_->memory_types();
    REXLOG_INFO("[native] C6: upload buffer in memory type {} ({}, {}; bo2zm_native_upload_memory = {})",
                upload_type_, ((types.host_cached >> upload_type_) & 1) ? "with CPU cache" : "without CPU cache",
                upload_coherent_ ? "coherent, not published" : "published before submitting",
                REXCVAR_GET(bo2zm_native_upload_memory));
    MeasureMemoryUpload();
    CreateSharedSeparate();
    CreateVertexPools();  // bo2zm_native_vertex_cache
    UseSlot(0);
    return true;
  }

  // --- bo2zm_native_vertex_cache (see the cvar) --------------------------------------------------------------------
  struct VertexCacheEntry;  // with the members, below
  // An offset in offsets_vertices with kVertexPoolTag is in vpools_[(offset >> 61) & 1], not in the upload buffer.
  static constexpr VkDeviceSize kVertexPoolTag = VkDeviceSize(1) << 62;
  static constexpr VkDeviceSize kVertexPoolIndex = VkDeviceSize(1) << 61;
  static constexpr VkDeviceSize kVertexOffsetMask = kVertexPoolIndex - 1;
  // The submissions a pool waits, once left, before it is filled again (3 work slots, with margin).
  static constexpr uint64_t kVertexPoolRest = 8;

  void ResolveVertexBinding(VkDeviceSize tagged, VkBuffer upload, VkBuffer& buffer, VkDeviceSize& offset) const {
    if (tagged & kVertexPoolTag) {
      buffer = vpools_[(tagged & kVertexPoolIndex) ? 1 : 0].buffer;
      offset = tagged & kVertexOffsetMask;
    } else {
      buffer = upload;
      offset = tagged;
    }
  }

  void CreateVertexPools() {
    vcache_ab_ = REXCVAR_GET(bo2zm_native_vertex_cache_ab);
    vcache_sum_ab_ = REXCVAR_GET(bo2zm_native_vertex_cache_sum_ab);
    const VkDeviceSize size = VkDeviceSize(std::clamp(REXCVAR_GET(bo2zm_native_vertex_cache_mb), 4, 256)) << 20;
    for (VertexPool& pool : vpools_) {
      // In the upload buffer's memory type (written by the CPU, read by the GPU as vertices); index cache too.
      gpu::vulkan::HostBuffer created;
      const gpu::vulkan::HostBufferResult result = gpu::vulkan::CreateHostBuffer(
          dfn_, device_, nullptr, size, gpu::kBufferVertex | gpu::kBufferIndex, upload_type_, created);
      pool.buffer = created.buffer;
      pool.memory = created.memory;
      if (result == gpu::vulkan::HostBufferResult::kNoBuffer) {
        REXLOG_WARN("[native] C6 vertex cache: vkCreateBuffer failed; the cache is off");
        return;
      }
      if (result == gpu::vulkan::HostBufferResult::kWrongMemory) {
        REXLOG_WARN("[native] C6 vertex cache: the upload memory type does not fit a vertex buffer; the cache is off");
        return;
      }
      if (result != gpu::vulkan::HostBufferResult::kDone) {
        REXLOG_WARN("[native] C6 vertex cache: no memory for {} MB; the cache is off", size >> 20);
        return;
      }
      pool.data = created.data;
      pool.size = size;
      pool.size_real = created.size_real;
    }
    vcache_coherent_ = upload_coherent_;
    vcache_ready_ = true;
    vcache_by_writes_ = REXCVAR_GET(bo2zm_native_vertex_cache_by_writes);
    if (vcache_by_writes_) {
      bo2zm::page_writes::Enable(true);  // from now on, including the maps loaded later
    }
    REXLOG_INFO("[native] C6 vertex cache (bo2zm_native_vertex_cache): two pools of {} MB in memory type {}{}", size >> 20,
                upload_type_, vcache_ab_ ? "; A/B factor 1 in the benchmark" : "");
  }

  // The range's persistent copy, if it has one or should get one now; false: copy it into the upload buffer as
  // usual. hash: its XXH3 | 1 when the caller already has it (the dedupe's check frames), else 0. With
  // vcache_sum_ the check is ChangeSum instead (bo2zm_native_vertex_cache_sum); a pool copy carries both, so the A/B
  // can switch between them without dropping the copies.
  // PC diagnostics (bo2zm_debug_watch_vertices): the pages the vertex copies read during frames
  // 3000-3300, then the write watch on them, to find which game code writes vertex data.
  bool watch_vertices_ = bo2zm::debug::WatchVertices();
  std::unordered_map<uint32_t, uint64_t> watch_vertex_pages_;  // page -> bytes copied from it
  void WatchVerticesCollect(uint64_t address, uint32_t bytes) {
    constexpr uint64_t kFrom = 3000, kUntil = 3300;
    if (frame_ < kFrom) {
      return;
    }
    if (frame_ >= kUntil) {
      std::vector<uint32_t> pages;
      std::vector<uint64_t> copied;
      uint64_t total = 0;
      for (const auto& [page, b] : watch_vertex_pages_) {
        pages.push_back(page);
        copied.push_back(b);
        total += b;
      }
      REXLOG_INFO("[native] vertex pages read in frames {}-{}: {} ({:.1f} MB), {:.1f} MB copied from them; watching "
                  "who writes them",
                  kFrom, kUntil, pages.size(), double(pages.size()) * 4096.0 / 1e6, double(total) / 1e6);
      bo2zm::debug::WatchPhysicalPages(pages, bo2zm::debug::Watch::kVertices, &copied);
      watch_vertices_ = false;
      return;
    }
    // Each page gets the bytes of the range that fall in it.
    const uint64_t end = address + bytes;
    for (uint64_t at = address; at < end;) {
      const uint64_t page_end = (at & ~uint64_t(4095)) + 4096;
      const uint64_t take = std::min(end, page_end) - at;
      watch_vertex_pages_[uint32_t(at) & 0x1FFFF000u] += take;
      at += take;
    }
  }

  bool VertexCache(const uint8_t* data, uint32_t bytes, xenos::Endian order, uint64_t address,
                   VkDeviceSize alignment, uint64_t& hash, VkDeviceSize& tagged) {
    if (vcache_by_writes_) {
      return VertexCacheByWrites(data, bytes, order, address, alignment, tagged);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const bool by_sum = vcache_sum_;
    if (by_sum) {
      hash = ChangeSum(data, size_t(bytes)) | 1;
    } else if (hash == 0) {
      hash = XXH3_64bits(data, size_t(bytes)) | 1;
    }
    vcache_ns_hash_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count());
    vcache_bytes_hashed_ += bytes;
    const uint64_t key = (address & 0x1FFFFFFF) | (uint64_t(bytes & 0x3FFFFFF) << 29) | (uint64_t(order) << 55) |
                         (uint64_t(1) << 63);  // never 0 (an empty slot)
    VertexCacheEntry& e = VertexCacheSlot(key);
    if ((by_sum ? e.sum_pool : e.hash_pool) == hash && e.generation == vpool_generation_[e.pool]) {
      ++vcache_hits_;
      vcache_bytes_hit_ += bytes;
      tagged = kVertexPoolTag | (e.pool ? kVertexPoolIndex : 0) | e.offset;
      GuardVertexCache(data, bytes, order, vpools_[e.pool].data + e.offset);
      return true;
    }
    if (e.hash_seen != hash || e.seen_by_sum != by_sum) {
      e.hash_seen = hash;  // new or changed: copied as usual this time
      e.seen_by_sum = by_sum;
      ++vcache_changed_;
      return false;
    }
    // The same bytes as last time: one copy into the pool being filled.
    VkDeviceSize at = 0;
    if (!ReserveVertexPool(bytes, alignment, at)) {
      return false;
    }
    VertexPool& pool = vpools_[vpool_];
    const CopyJob job{data, pool.data + at, bytes / 4, order};
    if (!copies_active_ || !EnqueueCopy(job)) {
      CopyVertices(job);
    }
    pool.dirty_lo = std::min(pool.dirty_lo, at);
    pool.dirty_hi = std::max(pool.dirty_hi, at + bytes);
    // Both checks for the copy (a fill is rare: the second one costs one more read of the range).
    e.hash_pool = by_sum ? XXH3_64bits(data, size_t(bytes)) | 1 : hash;
    e.sum_pool = by_sum ? hash : ChangeSum(data, size_t(bytes)) | 1;
    e.pool = vpool_;
    e.offset = at;
    e.generation = vpool_generation_[vpool_];
    ++vcache_filled_;
    vcache_bytes_filled_ += bytes;
    tagged = kVertexPoolTag | (vpool_ ? kVertexPoolIndex : 0) | at;
    return true;
  }

  // bo2zm_native_vertex_cache_by_writes. A range drawn twice with no write stamped on its pages in between is copied
  // once into the pool being filled, in the epoch of that copy; later draws bind that copy while no page has a stamp
  // from that epoch on. A range just seen, or written since, is copied into the upload buffer as usual.
  bool VertexCacheByWrites(const uint8_t* data, uint32_t bytes, xenos::Endian order, uint64_t address,
                           VkDeviceSize alignment, VkDeviceSize& tagged) {
    const uint32_t physical = uint32_t(address) & 0x1FFFFFFFu;
    const uint64_t key = (address & 0x1FFFFFFF) | (uint64_t(bytes & 0x3FFFFFF) << 29) | (uint64_t(order) << 55) |
                         (uint64_t(1) << 63);
    auto t_part = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    VertexCacheEntry& e = VertexCacheSlot(key);
    CutSubstage(45, t_part);  // a lookup's parts (C6 substages 45-47)
    const uint32_t epoch = bo2zm::page_writes::Epoch();
    const bool unchanged = e.copy_epoch != 0 && e.generation == vpool_generation_[e.pool] &&
                           bo2zm::page_writes::Unchanged(physical, bytes, e.copy_epoch);
    CutSubstage(46, t_part);
    if (unchanged) {
      const bool guard_passed = GuardVertexCacheByWrites(data, bytes, physical, e.sum_pool, e.hits++);
      CutSubstage(47, t_part);
      if (!guard_passed) {
        bo2zm::page_writes::MarkVolatile(physical, bytes);
        e.copy_epoch = 0;
        e.seen_epoch = 0;
        return false;
      }
      ++vcache_hits_;
      vcache_bytes_hit_ += bytes;
      tagged = kVertexPoolTag | (e.pool ? kVertexPoolIndex : 0) | e.offset;
      return true;
    }
    if (e.seen_epoch == 0 || !bo2zm::page_writes::Unchanged(physical, bytes, e.seen_epoch)) {
      e.seen_epoch = epoch;  // new, or written since it was last drawn: copied as usual this time
      e.copy_epoch = 0;
      ++vcache_changed_;
      return false;
    }
    VkDeviceSize at = 0;
    if (!ReserveVertexPool(bytes, alignment, at)) {
      return false;
    }
    VertexPool& pool = vpools_[vpool_];
    const CopyJob job{data, pool.data + at, bytes / 4, order};
    if (!copies_active_ || !EnqueueCopy(job)) {
      CopyVertices(job);
    }
    pool.dirty_lo = std::min(pool.dirty_lo, at);
    pool.dirty_hi = std::max(pool.dirty_hi, at + bytes);
    e.pool = vpool_;
    e.offset = at;
    e.generation = vpool_generation_[vpool_];
    e.copy_epoch = epoch;
    e.hits = 0;
    e.sum_pool = ChangeSum(data, size_t(bytes)) | 1;  // what the guard compares with, without waiting for the copy
    ++vcache_filled_;
    vcache_bytes_filled_ += bytes;
    tagged = kVertexPoolTag | (vpool_ ? kVertexPoolIndex : 0) | at;
    return true;
  }


  /*
   * GPU skinning (bo2zm_gpu_skin.h, shaders/bo2zm_gpu_skin.comp). The vertex range [address, + bytes)
   * (guest bytes at data, physical address) holds output of the jobs in skin_jobs_. Its output is reserved where the
   * copy would have put it, and each job's part becomes a record in this submission's job table: a block in the
   * upload buffer with the job's constants and bone snapshot (copied with the vertex copy's k8in32 swap, so the shader
   * reads values), and the model's records and input vertices from the input cache (below), or copied after the block
   * when they are not in it. BeforeSubmit records one dispatch over the table's workgroups in the upload command
   * buffer, which is submitted before the job buffer, and the barrier (DispatchSkinTable). What no job covers is
   * copied as usual. A range the shader cannot write as the copy would have (another stride or byte order, a job that
   * does not start on one of its vertices, no room) has its jobs computed on the CPU into guest memory and is copied
   * as usual.
   *
   * The input cache: a job's records and input vertices are its model's data, the same for every zombie with that
   * model and in every frame. A job's whole inputs are copied once into the pool being filled (two persistent pools)
   * and stay valid while no page they come from has a write stamp since the copy (bo2zm_page_writes.h, as the vertex
   * cache); the first 2 uses of a copy and every 256th after compare the guest bytes' ChangeSum with the copy's. A full
   * pool hands over to the other one once that one has rested kVertexPoolRest submissions; both pools' copies are
   * dropped then (as the vertex pools: the one left is never read again before it has rested).
   */
  struct SkinJobRecord {  // the shader's JobRecord (std430)
    VkDeviceAddress block;
    VkDeviceAddress inputs;
    VkDeviceAddress target;
    uint32_t count, kernel, bones_word, records_byte, vertices_word, bones_guest, bones_lo, unused;
  };
  static_assert(sizeof(SkinJobRecord) == 56, "the shader's job record");
  struct SkinPush {
    VkDeviceAddress jobs;
    VkDeviceAddress groups;
  };
  static constexpr uint32_t kSkinTableJobs = 4096;     // job records per submission
  static constexpr uint32_t kSkinTableGroups = 32768;  // workgroups of 64 vertices per submission
  static constexpr VkDeviceSize kSkinTableBytes = VkDeviceSize(kSkinTableJobs) * sizeof(SkinJobRecord) +
                                                  VkDeviceSize(kSkinTableGroups) * 8;
  // Each of the input cache's two pools. By round 15 in Town they hold ~1 MB in all.
  static constexpr VkDeviceSize kSkinPoolBytes = VkDeviceSize(8) << 20;

  // A host-visible buffer the shader reaches by its address (the upload buffer's memory type).
  bool CreateSkinBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& data,
                        VkDeviceAddress& address) {
    gpu::vulkan::HostBuffer created;
    const gpu::vulkan::HostBufferResult result = gpu::vulkan::CreateHostBuffer(
        dfn_, device_, address_buffer_, size, gpu::kBufferStorage | gpu::kBufferAddress, upload_type_, created);
    buffer = created.buffer;
    memory = created.memory;
    if (created.data) data = created.data;
    address = created.address;
    return result == gpu::vulkan::HostBufferResult::kDone;
  }

  void CreateGpuSkin() {
    if (!bo2zm::gpu_skin::RendererOn()) {
      return;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = sizeof(kSpirvBo2zmGpuSkin);
    info.pCode = kSpirvBo2zmGpuSkin;
    VkShaderModule module = VK_NULL_HANDLE;
    if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
      REXLOG_ERROR("[gpu_skin] the compute shader could not be created: skinning stays on the CPU");
      return;
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPush)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    if (dfn_.vkCreatePipelineLayout(device_, &layout, nullptr, &skin_layout_) == VK_SUCCESS) {
      VkComputePipelineCreateInfo pipeline{};
      pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      pipeline.stage.module = module;
      pipeline.stage.pName = "main";
      pipeline.layout = skin_layout_;
      if (dfn_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &skin_pipeline_) !=
          VK_SUCCESS) {
        skin_pipeline_ = VK_NULL_HANDLE;
      }
    }
    dfn_.vkDestroyShaderModule(device_, module, nullptr);
    const bool buffers =
        skin_pipeline_ != VK_NULL_HANDLE &&
        CreateSkinBuffer(kSkinTableBytes * kSlotsOfJob, skin_table_buffer_, skin_table_memory_, skin_table_data_,
                         skin_table_address_) &&
        CreateSkinBuffer(kSkinPoolBytes * 2, skin_pool_buffer_, skin_pool_memory_, skin_pool_data_,
                         skin_pool_address_);
    skin_on_ = buffers;
    bo2zm::gpu_skin::SetRendererReady(skin_on_);
    REXLOG_INFO("[gpu_skin] renderer: {}", skin_on_ ? "the compute pipeline and its buffers are ready, the hooks may "
                                                      "record jobs"
                                                    : "no compute pipeline or buffers: skinning stays on the CPU");
  }

  // The job's whole inputs in the input cache (its records from the word that holds the first one, then its input
  // vertices): their address, or 0 when they must be copied per use. records_words: the records' words there.
  VkDeviceAddress SkinCachedInputs(const bo2zm::gpu_skin::Job& job, uint32_t halves, uint32_t& records_words) {
    bo2zm::gpu_skin::RendererCounts& counts = bo2zm::gpu_skin::Counts();
    records_words = ((job.records_guest & 3) + job.count * halves * 2 + 3) / 4;
    uint32_t vertices_physical = 0, records_physical = 0;
    if (!bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed) ||
        !bo2zm::gpu_skin::Physical(job.vertices_guest, vertices_physical) ||
        !bo2zm::gpu_skin::Physical(job.records_guest & ~3u, records_physical)) {
      return 0;
    }
    const uint32_t vertices_bytes = job.count * 24, records_bytes = records_words * 4;
    const uint8_t* records = job.records - (job.records_guest & 3);
    const uint64_t key = uint64_t(job.vertices_guest) | (uint64_t(job.count & 0x3FFFFFFF) << 32) |
                         (uint64_t(job.kernel) << 62);
    auto [it, fresh] = skin_cache_.try_emplace(key);
    SkinCacheEntry& e = it->second;
    if (!fresh && e.records_guest == job.records_guest && e.generation == skin_pools_[e.pool].generation &&
        bo2zm::page_writes::Unchanged(vertices_physical, vertices_bytes, e.epoch) &&
        bo2zm::page_writes::Unchanged(records_physical, records_bytes, e.epoch)) {
      const uint32_t use = e.uses++;
      if ((use >= 2 && (use & 255) != 0) ||
          (ChangeSum(job.vertices, vertices_bytes) ^ ChangeSum(records, records_bytes)) == e.sum) {
        counts.cache_hits.fetch_add(1, std::memory_order_relaxed);
        return skin_pool_address_ + e.pool * kSkinPoolBytes + e.offset;
      }
      counts.guard_fails.fetch_add(1, std::memory_order_relaxed);  // changed without a stamp: copied again
    }
    const VkDeviceSize need = (VkDeviceSize(records_bytes) + vertices_bytes + 15) & ~VkDeviceSize(15);
    SkinPool* pool = &skin_pools_[skin_pool_];
    if (pool->used + need > kSkinPoolBytes) {
      SkinPool& other = skin_pools_[skin_pool_ ^ 1];
      if (need > kSkinPoolBytes || frame_ - other.left < kVertexPoolRest) {
        skin_cache_.erase(it);
        return 0;
      }
      pool->left = frame_;
      ++pool->generation;  // its copies are not used again: the GPU is done with them once it has rested
      skin_pool_ ^= 1;
      pool = &other;
      ++pool->generation;
      pool->used = 0;
    }
    const VkDeviceSize at = pool->used;
    pool->used += need;
    uint8_t* target = skin_pool_data_ + skin_pool_ * kSkinPoolBytes + at;
    CopyVertices(CopyJob{records, target, records_words, xenos::Endian::k8in32});
    CopyVertices(CopyJob{job.vertices, target + records_bytes, job.count * 6, xenos::Endian::k8in32});
    pool->dirty_lo = std::min(pool->dirty_lo, at);
    pool->dirty_hi = std::max(pool->dirty_hi, at + need);
    e = {job.records_guest,
         skin_pool_,
         pool->generation,
         at,
         bo2zm::page_writes::Epoch(),
         0,
         ChangeSum(job.vertices, vertices_bytes) ^ ChangeSum(records, records_bytes)};
    counts.cache_fills.fetch_add(1, std::memory_order_relaxed);
    counts.cache_fill_bytes.fetch_add(need, std::memory_order_relaxed);
    return skin_pool_address_ + skin_pool_ * kSkinPoolBytes + at;
  }

  bool SkinOnGpu(const uint8_t* data, xenos::Endian order, uint32_t bytes, uint64_t address, uint32_t stride,
                 bool single, VkDeviceSize& offset) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t lo = uint32_t(address);
    const uint64_t hi = uint64_t(lo) + bytes;
    bo2zm::gpu_skin::Overlapping(lo, bytes, skin_jobs_);
    if (skin_jobs_.empty()) {
      return false;
    }
    bo2zm::gpu_skin::RendererCounts& counts = bo2zm::gpu_skin::Counts();
    static constexpr uint32_t kHalves[3] = {1, 3, 5};
    const char* refused = nullptr;
    VkDeviceSize space = VkDeviceSize(bytes) + stride + 16;  // the output, with ReserveMultiple's padding
    uint32_t parts = 0, groups = 0;
    if (stride != 32) {
      refused = "stride";
    } else if (order != xenos::Endian::k8in32) {
      refused = "byte order";
    } else {
      for (const auto& job : skin_jobs_) {
        const uint64_t first = std::max<uint64_t>(lo, job->out);
        const uint64_t last = std::min<uint64_t>(hi, job->out + uint64_t(job->count) * 32);
        if ((first - lo) % 32 || (first - job->out) % 32 || (last - first) % 32) {
          refused = "a job not on the range's vertices";
          break;
        }
        const uint64_t n = (last - first) / 32;
        // The block, and the inputs in case they are not in the input cache.
        space += 128 + job->bones.size() + n * kHalves[job->kernel] * 2 + 8 + n * 24 + 32 +
                 (job->cpu_written ? n * 32 + 16 : 0);
        parts += 1;
        groups += uint32_t((n + 63) / 64);
      }
    }
    SkinTable& table = skin_tables_[slot_current_];
    if (!refused && (table.jobs + parts > kSkinTableJobs || table.groups + groups > kSkinTableGroups)) {
      refused = "the submission's job table is full";
    }
    if (!refused && upload_used_ + space > kSizeUpload) {
      refused = "no room in the upload buffer";
    }
    if (refused) {
      bo2zm::gpu_skin::Refused(refused, lo, bytes, stride, uint32_t(order));
      counts.refused.fetch_add(1, std::memory_order_relaxed);
      for (const auto& job : skin_jobs_) {
        counts.refused_vertices.fetch_add(job->count, std::memory_order_relaxed);
        bo2zm::gpu_skin::Materialize(*job);
      }
      skin_jobs_.clear();
      return false;
    }
    if (vertices_base_cero_ && single) {
      ReserveMultiple(bytes, stride, offset);
    } else {
      Reserve(bytes, 4, offset);
    }
    const auto gap = [&](uint64_t from, uint64_t to) {
      CopyVertices(CopyJob{data + (from - lo), upload_data_ + offset + (from - lo), uint32_t(to - from) / 4, order});
      counts.gap_bytes.fetch_add(to - from, std::memory_order_relaxed);
    };
    uint8_t* const table_data = skin_table_data_ + slot_current_ * kSkinTableBytes;
    uint64_t cursor = lo, per_use = 0, vertices = 0;
    for (const auto& job : skin_jobs_) {
      const uint64_t first = std::max<uint64_t>(lo, job->out);
      const uint64_t last = std::min<uint64_t>(hi, job->out + uint64_t(job->count) * 32);
      if (first > cursor) {
        gap(cursor, first);
      }
      cursor = std::max(cursor, last);
      const uint32_t v0 = uint32_t((first - job->out) / 32), n = uint32_t((last - first) / 32);
      if (n == 0) {
        continue;
      }
      const uint32_t halves = kHalves[job->kernel];
      const uint32_t bones_bytes = uint32_t(job->bones.size());
      uint32_t records_words = 0;
      VkDeviceAddress inputs = SkinCachedInputs(*job, halves, records_words);
      // Not cached: the part's records, from the word that holds vertex v0's first one, and vertices after the bones.
      const uint32_t records_from = job->records_guest + v0 * halves * 2;
      const uint32_t records_word0 = records_from & ~3u;
      const uint32_t part_records_words = (records_from + n * halves * 2 - records_word0 + 3) / 4;
      const uint32_t block_bytes = 128 + bones_bytes + (inputs ? 0 : part_records_words * 4 + n * 24);
      VkDeviceSize block = 0;
      Reserve(block_bytes, 16, block);
      uint8_t* at = upload_data_ + block;
      std::memcpy(at, job->constants, 128);
      CopyVertices(CopyJob{job->bones.data(), at + 128, bones_bytes / 4, xenos::Endian::k8in32});
      SkinJobRecord record{};
      record.block = upload_address_ + block;
      record.target = upload_address_ + offset + (first - lo);
      record.count = n;
      record.kernel = job->kernel;
      record.bones_word = 32;
      record.bones_guest = job->bones_guest;
      record.bones_lo = job->bones_lo;
      if (inputs) {
        record.inputs = inputs;
        record.records_byte = (job->records_guest & 3) + v0 * halves * 2;
        record.vertices_word = records_words + v0 * 6;
      } else {
        CopyVertices(CopyJob{job->records + (int64_t(records_word0) - int64_t(job->records_guest)),
                             at + 128 + bones_bytes, part_records_words, xenos::Endian::k8in32});
        CopyVertices(CopyJob{job->vertices + size_t(v0) * 24, at + 128 + bones_bytes + part_records_words * 4, n * 6,
                             xenos::Endian::k8in32});
        record.inputs = upload_address_ + block + 128 + bones_bytes;
        record.records_byte = records_from & 3;
        record.vertices_word = part_records_words;
        counts.uncached.fetch_add(1, std::memory_order_relaxed);
      }
      std::memcpy(table_data + VkDeviceSize(table.jobs) * sizeof(SkinJobRecord), &record, sizeof(record));
      uint8_t* const group_data = table_data + VkDeviceSize(kSkinTableJobs) * sizeof(SkinJobRecord);
      for (uint32_t g = 0; g < n; g += 64) {
        const uint32_t entry[2] = {table.jobs, g};
        std::memcpy(group_data + VkDeviceSize(table.groups++) * 8, entry, 8);
      }
      ++table.jobs;
      per_use += block_bytes;
      vertices += n;
      counts.dispatches.fetch_add(1, std::memory_order_relaxed);
      if (job->cpu_written) {
        // bo2zm_gpu_skin_check: the CPU kernel's output, copied next to it, compared when the buffer comes back.
        VkDeviceSize cpu = 0;
        Reserve(VkDeviceSize(n) * 32, 16, cpu);
        CopyVertices(CopyJob{data + (first - lo), upload_data_ + cpu, n * 8, order});
        skin_checks_.push_back({slot_current_, offset + (first - lo), cpu, n, job->kernel, uint32_t(first)});
      }
    }
    if (cursor < hi) {
      gap(cursor, hi);
    }
    skin_jobs_.clear();
    counts.ranges.fetch_add(1, std::memory_order_relaxed);
    counts.vertices.fetch_add(vertices, std::memory_order_relaxed);
    counts.input_bytes.fetch_add(per_use, std::memory_order_relaxed);
    counts.ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count()),
                        std::memory_order_relaxed);
    return true;
  }

  // BeforeSubmit: the job table's workgroups not dispatched yet, as one dispatch in the upload command buffer, and
  // the barrier before the job buffer's draws read the vertices.
  void DispatchSkinTable() {
    SkinTable& table = skin_tables_[slot_current_];
    if (table.groups == table.dispatched) {
      return;
    }
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload) {
      return;
    }
    if (!upload_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, skin_table_memory_, upload_type_, 0,
                                                    VK_WHOLE_SIZE, VK_WHOLE_SIZE);
      for (uint32_t p = 0; p < 2; ++p) {
        SkinPool& pool = skin_pools_[p];
        if (pool.dirty_lo < pool.dirty_hi) {
          rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, skin_pool_memory_, upload_type_, 0,
                                                        VK_WHOLE_SIZE, VK_WHOLE_SIZE);
          break;
        }
      }
    }
    for (SkinPool& pool : skin_pools_) {
      pool.dirty_lo = ~VkDeviceSize(0);
      pool.dirty_hi = 0;
    }
    if (!skin_bound_) {
      gpu_.BindComputePipeline(upload, skin_pipeline_);
      skin_bound_ = true;
    }
    const VkDeviceAddress base = skin_table_address_ + slot_current_ * kSkinTableBytes;
    const SkinPush push{base, base + VkDeviceSize(kSkinTableJobs) * sizeof(SkinJobRecord) +
                                  VkDeviceSize(table.dispatched) * 8};
    gpu_.PushConstants(upload, skin_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    gpu_.Dispatch(upload, table.groups - table.dispatched, 1, 1);
    table.dispatched = table.groups;
    bo2zm::gpu_skin::Counts().submit_dispatches.fetch_add(1, std::memory_order_relaxed);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    gpu_.Barrier(upload, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1,
                 &barrier, 0, nullptr, 0, nullptr);
  }

  // bo2zm_gpu_skin_check: the checks recorded in upload buffer `index`, now that the GPU is done with it.
  void CheckGpuSkin(uint32_t index) {
    if (skin_checks_.empty()) {
      return;
    }
    const UploadBuffer& u = uploads_[index];
    bool invalidated = upload_coherent_;
    for (auto it = skin_checks_.begin(); it != skin_checks_.end();) {
      if (it->upload != index) {
        ++it;
        continue;
      }
      if (!invalidated) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = u.guest_memory;
        range.size = VK_WHOLE_SIZE;
        dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &range);
        invalidated = true;
      }
      bo2zm::gpu_skin::Compare(u.data + it->gpu, u.data + it->cpu, it->vertices, it->kernel, it->physical);
      it = skin_checks_.erase(it);
    }
  }

  /*
   * bo2zm_native_index_cache. The vertex cache by write stamps
   * (VertexCacheByWrites) for a draw's 16-bit index range: same table (key bit 62 tells index ranges apart), same
   * pools, same epochs. A range drawn twice with no write stamped on its pages in between is converted (byte order,
   * minimum and maximum: Indices16) once into the pool being filled; later draws take its minimum and maximum from the
   * entry and bind the copy while no page has a newer stamp. The guard is the vertex cache's: the guest bytes'
   * ChangeSum against the one taken at the copy, on the first 2 hits of each copy, every 64th after, and every hit in
   * the first 2,000.
   */
  bool IndexCacheByWrites(const uint8_t* data, uint32_t count, bool flip, uint32_t physical, uint32_t order,
                          uint32_t& min, uint32_t& max, VkDeviceSize& tagged) {
    const uint32_t bytes = count * 2;
    const uint64_t key = physical | (uint64_t(count & 0x3FFFFFF) << 29) | (uint64_t(order & 3) << 55) |
                         (uint64_t(1) << 62) | (uint64_t(1) << 63);
    auto t_part = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    VertexCacheEntry& e = VertexCacheSlot(key);
    CutSubstage(45, t_part);
    const uint32_t epoch = bo2zm::page_writes::Epoch();
    const bool unchanged = e.copy_epoch != 0 && e.generation == vpool_generation_[e.pool] &&
                           bo2zm::page_writes::Unchanged(physical, bytes, e.copy_epoch);
    CutSubstage(46, t_part);
    if (unchanged) {
      const bool guard_passed = GuardIndexCache(data, bytes, physical, e.sum_pool, e.hits++);
      CutSubstage(47, t_part);
      if (!guard_passed) {
        bo2zm::page_writes::MarkVolatile(physical, bytes);
        e.copy_epoch = 0;
        e.seen_epoch = 0;
        return false;
      }
      min = e.min;
      max = e.max;
      ++icache_hits_;
      icache_bytes_hit_ += bytes;
      tagged = kVertexPoolTag | (e.pool ? kVertexPoolIndex : 0) | e.offset;
      return true;
    }
    if (e.seen_epoch == 0 || !bo2zm::page_writes::Unchanged(physical, bytes, e.seen_epoch)) {
      e.seen_epoch = epoch;  // new, or written since it was last drawn: scanned and uploaded as usual this time
      e.copy_epoch = 0;
      ++icache_changed_;
      return false;
    }
    VkDeviceSize at = 0;
    if (!ReserveVertexPool(bytes, 4, at)) {
      ++icache_full_;
      return false;
    }
    VertexPool& pool = vpools_[vpool_];
    uint32_t mn = 0;
    uint32_t mx = 0;
    Indices16(data, count, reinterpret_cast<uint16_t*>(pool.data + at), flip, mn, mx);
    pool.dirty_lo = std::min(pool.dirty_lo, at);
    pool.dirty_hi = std::max(pool.dirty_hi, at + bytes);
    e.pool = vpool_;
    e.offset = at;
    e.generation = vpool_generation_[vpool_];
    e.copy_epoch = epoch;
    e.hits = 0;
    e.sum_pool = ChangeSum(data, size_t(bytes)) | 1;
    e.min = uint16_t(mn);
    e.max = uint16_t(mx);
    min = mn;
    max = mx;
    ++icache_filled_;
    icache_bytes_filled_ += bytes;
    tagged = kVertexPoolTag | (vpool_ ? kVertexPoolIndex : 0) | at;
    return true;
  }

  bool GuardIndexCache(const uint8_t* data, uint32_t bytes, uint32_t physical, uint64_t expected, uint32_t hit) {
    const uint64_t n = ++icache_guard_seen_;
    if (n > 2000 && hit >= 2 && (hit & 63) != 0) {
      return true;
    }
    ++icache_checked_;
    if ((ChangeSum(data, size_t(bytes)) | 1) == expected) {
      return true;
    }
    if (++icache_unstamped_ <= 16) {
      REXLOG_WARN("[native] C6 index cache by writes: physical {:08X}+{} changed without a write stamp (a writer "
                  "that does not stamp); its pages now count as written every frame",
                  physical, bytes);
    }
    ++icache_unstamped_interval_;
    return false;
  }

  void ReportIndexCache() {
    if (icache_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < icache_next_) {
      return;
    }
    const bool first = icache_next_ == std::chrono::steady_clock::time_point{};
    icache_next_ = now + std::chrono::seconds(10);
    if (!first) {
      BO2ZM_REPORT_RING("[native] C6 index cache by write stamps, last 10 s: {} hits ({:.1f} MB not scanned or "
                        "uploaded), {} new copies in the pools ({:.1f} MB), {} changed or new ranges scanned as usual, "
                        "{} without room; {} hits checked, {} changed without a stamp ({} since start)",
                        icache_hits_, double(icache_bytes_hit_) / 1048576.0, icache_filled_,
                        double(icache_bytes_filled_) / 1048576.0, icache_changed_, icache_full_, icache_checked_,
                        icache_unstamped_interval_, icache_unstamped_);
    }
    icache_hits_ = icache_bytes_hit_ = icache_filled_ = icache_bytes_filled_ = icache_changed_ = icache_full_ = 0;
    icache_checked_ = icache_unstamped_interval_ = 0;
  }

  // Before a hit is bound, the guest bytes' ChangeSum is compared with the one taken when the pool copy was made, for:
  // the first 2 hits of each copy, then every 64th hit of that range, and every hit in the first 2,000. False when
  // they differ: a writer that does not stamp changed them (logged, the first 16), and the range is copied as usual
  // this time, so a check never lets a stale copy through. Per range, because on the PC all such writes were to one
  // region the game rewrites with plain stores (0x1BB14000-0x1BBC3000), each range caught at its first checked hit.
  // By sum, not by reading the pool copy back: that would have to wait for the copy thread first, and on the console
  // the ~4,000 checks a second (pools refilled after each switch) cost 9.5 % of the frame rate that way.
  bool GuardVertexCacheByWrites(const uint8_t* data, uint32_t bytes, uint32_t physical, uint64_t expected,
                                uint32_t hit) {
    const uint64_t n = ++vcache_guard_seen_;
    if (n > 2000 && hit >= 2 && (hit & 63) != 0) {
      return true;
    }
    ++vcache_checked_;
    if ((ChangeSum(data, size_t(bytes)) | 1) == expected) {
      return true;
    }
    if (++vcache_unstamped_ <= 16) {
      REXLOG_WARN("[native] C6 vertex cache by writes: physical {:08X}+{} changed without a write stamp (a writer "
                  "that does not stamp); its pages now count as written every frame",
                  physical, bytes);
    }
    ++vcache_unstamped_interval_;
    return false;
  }

  // bo2zm_native_prefetch_cache_lines: the set VertexCacheSlot will look at for this key, and the write stamp of the
  // range's first page.
  void PrefetchCacheLines(uint64_t key, uint32_t physical) {
    if (vcache_.empty()) {
      return;
    }
    const uint64_t mixed = (key ^ (key >> 29)) * 0x9E3779B97F4A7C15ull;
    const char* set = reinterpret_cast<const char*>(&vcache_[(mixed >> 40) % kVertexCacheSets * 4]);
    for (size_t at = 0; at < 4 * sizeof(VertexCacheEntry); at += 64) {
      __builtin_prefetch(set + at);
    }
    __builtin_prefetch(&bo2zm::page_writes::g_stamps[(physical & 0x1FFFFFFFu) >> 12]);
  }

  // The table: 4-way sets, no allocation. A range found in its set is used (and stamped); a new one takes the
  // set's oldest slot, so a range that has not been drawn for a while may lose its entry (and is hashed again from
  // scratch: copied as usual once, then into the pool).
  VertexCacheEntry& VertexCacheSlot(uint64_t key) {
    if (vcache_.empty()) {
      vcache_.resize(kVertexCacheSets * 4);
    }
    const uint64_t mixed = (key ^ (key >> 29)) * 0x9E3779B97F4A7C15ull;
    VertexCacheEntry* set = &vcache_[(mixed >> 40) % kVertexCacheSets * 4];
    VertexCacheEntry* oldest = set;
    for (int way = 0; way < 4; ++way) {
      if (set[way].key == key) {
        set[way].stamp = frame_;
        return set[way];
      }
      if (set[way].stamp < oldest->stamp) {
        oldest = &set[way];
      }
    }
    if (oldest->key != 0) {
      ++vcache_evicted_;
    }
    *oldest = VertexCacheEntry{};
    oldest->key = key;
    oldest->stamp = frame_;
    return *oldest;
  }

  // Room in the pool being filled; when it is full, the other one (if it has rested), dropping the full one's copies.
  bool ReserveVertexPool(uint32_t bytes, VkDeviceSize alignment, VkDeviceSize& at) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      VertexPool& pool = vpools_[vpool_];
      const VkDeviceSize start = (pool.used + alignment - 1) / alignment * alignment;
      if (start + bytes <= pool.size) {
        pool.used = start + bytes;
        at = start;
        return true;
      }
      const uint32_t other = vpool_ ^ 1;
      if (frame_ < vpools_[other].frame_left + kVertexPoolRest) {
        ++vcache_full_;
        return false;
      }
      pool.frame_left = frame_;
      ++vpool_generation_[vpool_];  // its copies are not bound again
      vpool_ = other;
      ++vpool_generation_[other];
      vpools_[other].used = 0;
      ++vcache_switches_;
    }
    return false;
  }

  // Before a submission: the copies made into the pools are flushed (non-coherent memory), after WaitUploads.
  void PublishVertexPools() {
    for (VertexPool& pool : vpools_) {
      if (pool.dirty_hi > pool.dirty_lo) {
        if (!vcache_coherent_) {
          const VkDeviceSize lo = pool.dirty_lo & ~VkDeviceSize(255);
          rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, pool.memory, upload_type_, lo, pool.size_real,
                                                        pool.dirty_hi - lo);
        }
        pool.dirty_lo = ~VkDeviceSize(0);
        pool.dirty_hi = 0;
      }
    }
  }

  // The first 2,000 hits, then 1 in 1,024: the guest bytes copied again, compared with the pool's copy. It waits for
  // the copy jobs first (a fill may still be on the copy thread).
  void GuardVertexCache(const uint8_t* data, uint32_t bytes, xenos::Endian order, const uint8_t* pool_copy) {
    const uint64_t n = ++vcache_guard_seen_;
    if (n > 2000 && (n & 1023) != 0) {
      return;
    }
    WaitUploads();
    vcache_scratch_.resize(bytes);
    CopyVertices(CopyJob{data, vcache_scratch_.data(), bytes / 4, order});
    ++vcache_checked_;
    if (std::memcmp(vcache_scratch_.data(), pool_copy, bytes) != 0) {
      vcache_off_ = true;
      vcache_active_ = false;
      REXLOG_ERROR("[native] C6 vertex cache: MISMATCH ({} bytes, order {}): the cache is off for this session", bytes,
                   uint32_t(order));
    }
  }

  void ReportVertexCache() {
    if (!vcache_ready_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - vcache_report_ < std::chrono::seconds(10)) {
      return;
    }
    vcache_report_ = now;
    BO2ZM_REPORT_RING("[native] C6 vertex cache (bo2zm_native_vertex_cache, {}), last 10 s: {} hits ({:.1f} MB not "
                      "copied), {} new copies in the pools ({:.1f} MB), {} changed or new ranges copied as usual; "
                      "hashed {:.1f} MB in {:.1f} ms; pool {} at {:.1f} of {} MB, {} switches, {} times full; {} "
                      "entries replaced by newer ranges; {} hits checked against the guest bytes",
                      vcache_active_ ? "on" : (vcache_off_ ? "OFF after a mismatch" : "off"), vcache_hits_,
                      double(vcache_bytes_hit_) / 1048576.0, vcache_filled_, double(vcache_bytes_filled_) / 1048576.0,
                      vcache_changed_, double(vcache_bytes_hashed_) / 1048576.0, double(vcache_ns_hash_) / 1e6, vpool_,
                      double(vpools_[vpool_].used) / 1048576.0, vpools_[vpool_].size >> 20, vcache_switches_,
                      vcache_full_, vcache_evicted_, vcache_checked_);
    if (vcache_by_writes_) {
      BO2ZM_REPORT_RING("[native] C6 vertex cache by write stamps: {} ranges changed without a stamp in 10 s ({} since "
                        "start), now copied every frame",
                        vcache_unstamped_interval_, vcache_unstamped_);
      vcache_unstamped_interval_ = 0;
    }
    vcache_hits_ = vcache_bytes_hit_ = vcache_filled_ = vcache_bytes_filled_ = vcache_changed_ = 0;
    vcache_bytes_hashed_ = vcache_ns_hash_ = vcache_full_ = vcache_evicted_ = 0;
  }

  // A small CPU-cached buffer per slot, only for the shared constants. If anything fails, it continues as
  // before (in the upload buffer) and says so in the log.
  void CreateSharedSeparate() {
    shared_separate_ = false;
    if (!REXCVAR_GET(bo2zm_native_shared_constants_cache)) {
      REXLOG_INFO("[native] C6: shared constants in the upload buffer (bo2zm_native_shared_constants_cache = false)");
      return;
    }
    const auto& types = vulkan_device_->memory_types();
    for (UploadBuffer& c : shared_bufs_) {
      VkBufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      info.size = kSizeShared;
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (dfn_.vkCreateBuffer(device_, &info, nullptr, &c.buffer) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not create the shared constants buffer; they go in the upload buffer");
        return;
      }
      VkMemoryRequirements requirements;
      dfn_.vkGetBufferMemoryRequirements(device_, c.buffer, &requirements);
      uint32_t type = 0;
      if (!rex::bit_scan_forward(requirements.memoryTypeBits & types.host_visible & types.host_cached, &type)) {
        REXLOG_WARN("[native] C6: no visible memory with CPU cache; the shared constants go in the upload buffer");
        return;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = requirements.size;
      reserve.memoryTypeIndex = type;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &c.guest_memory) != VK_SUCCESS ||
          dfn_.vkBindBufferMemory(device_, c.buffer, c.guest_memory, 0) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not allocate the shared constants buffer; they go in the upload buffer");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, c.guest_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not map the shared constants buffer; they go in the upload buffer");
        return;
      }
      c.data = static_cast<uint8_t*>(mapped);
      c.size_real = requirements.size;
      shared_type_ = type;
    }
    shared_coherent_ = (types.host_coherent >> shared_type_) & 0x1;
    shared_separate_ = true;
    REXLOG_INFO("[native] C6: shared constants separate, in memory type {} (with CPU cache, {}), {} MB per slot",
                shared_type_, shared_coherent_ ? "coherent" : "published before submitting",
                kSizeShared >> 20);
  }

  bool CreateBufferUpload() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kSizeUpload;
    // UNIFORM_BUFFER because it is also bound as a dynamic UBO (constants through UBOs, set 4).
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &upload_) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements;
    dfn_.vkGetBufferMemoryRequirements(device_, upload_, &requirements);
    // bo2zm_native_upload_memory: with 1 or 2, a host-visible type with or without CPU caching is looked
    // for; if there is none, the SDK's.
    const auto& types = vulkan_device_->memory_types();
    upload_type_ = UINT32_MAX;
    const int32_t preference = REXCVAR_GET(bo2zm_native_upload_memory);
    if (preference == 1 || preference == 2) {
      const uint32_t visible = requirements.memoryTypeBits & types.host_visible;
      uint32_t type = 0;
      if (rex::bit_scan_forward(visible & (preference == 1 ? types.host_cached : ~types.host_cached), &type)) {
        upload_type_ = type;
      }
    }
    if (upload_type_ == UINT32_MAX) {
      upload_type_ = rex::ui::vulkan::util::ChooseMemoryType(types, requirements.memoryTypeBits,
                                                             rex::ui::vulkan::util::MemoryPurpose::kUpload);
    }
    if (upload_type_ == UINT32_MAX) {
      return false;
    }
    upload_coherent_ = (vulkan_device_->memory_types().host_coherent >> upload_type_) & 0x1;
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reserve.pNext = &flags;
    reserve.allocationSize = requirements.size;
    reserve.memoryTypeIndex = upload_type_;
    if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &upload_memory_) != VK_SUCCESS) {
      return false;
    }
    upload_size_real_ = requirements.size;
    if (dfn_.vkBindBufferMemory(device_, upload_, upload_memory_, 0) != VK_SUCCESS) {
      return false;
    }
    void* mapped = nullptr;
    if (dfn_.vkMapMemory(device_, upload_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    upload_data_ = static_cast<uint8_t*>(mapped);
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = upload_;
    upload_address_ = address_buffer_(device_, &address);
    return upload_address_ != 0;
  }

  // For the bo2zm_native_upload_memory A/B test: CPU write MB/s into each host-visible memory type and the
  // cost of publishing those 8 MB (vkFlushMappedMemoryRanges; in NVK for Tegra, armDCacheClean). Once.
  void MeasureMemoryUpload() {
    if (!REXCVAR_GET(bo2zm_native_diag_upload_memory)) {
      return;
    }
    constexpr VkDeviceSize kBytes = VkDeviceSize(8) << 20;
    std::vector<uint8_t> source(static_cast<size_t>(kBytes));
    for (size_t i = 0; i < source.size(); ++i) {
      source[i] = uint8_t((i * 131) ^ (i >> 11));
    }
    const auto& types = vulkan_device_->memory_types();
    std::string report;
    for (uint32_t type = 0; type < 32; ++type) {
      if (!((types.host_visible >> type) & 1)) {
        continue;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = kBytes;
      reserve.memoryTypeIndex = type;
      VkDeviceMemory guest_memory = VK_NULL_HANDLE;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &guest_memory) != VK_SUCCESS) {
        continue;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, guest_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        using Clock = std::chrono::steady_clock;
        std::memcpy(mapped, source.data(), size_t(kBytes));  // first pass: pages and caches
        const auto before = Clock::now();
        std::memcpy(mapped, source.data(), size_t(kBytes));
        const auto copied = Clock::now();
        const bool coherent = (types.host_coherent >> type) & 1;
        if (!coherent) {
          VkMappedMemoryRange range{};
          range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
          range.memory = guest_memory;
          range.size = VK_WHOLE_SIZE;
          dfn_.vkFlushMappedMemoryRanges(device_, 1, &range);
        }
        const auto published = Clock::now();
        const double seconds = std::chrono::duration<double>(copied - before).count();
        report += fmt::format(
            "{}type {} ({}{}): {:.0f} MB/s{}", report.empty() ? "" : "; ", type,
            ((types.host_cached >> type) & 1) ? "with CPU cache" : "without CPU cache", coherent ? ", coherent" : "",
            seconds > 0 ? 8.0 / seconds : 0.0,
            coherent ? std::string()
                      : fmt::format(", publish {:.2f} ms",
                                    std::chrono::duration<double, std::milli>(published - copied).count()));
        dfn_.vkUnmapMemory(device_, guest_memory);
      }
      dfn_.vkFreeMemory(device_, guest_memory, nullptr);
    }
    REXLOG_INFO("[native] C6: CPU write speed per memory type (8 MB): {}",
                report.empty() ? std::string("no measurable types") : report);
  }

  bool CreateDescriptors() {
    static constexpr VkDescriptorType kTypes[4] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    const VkDescriptorBindingFlags flags_binding =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = kTypes[i];
      binding.descriptorCount = kCapacityHeap[i];
      binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};
      flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      flags.bindingCount = 1;
      flags.pBindingFlags = &flags_binding;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.pNext = &flags;
      info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
      info.bindingCount = 1;
      info.pBindings = &binding;
      if (dfn_.vkCreateDescriptorSetLayout(device_, &info, nullptr, &layouts_[i]) != VK_SUCCESS) {
        return false;
      }
      heaps_[i].capacity = kCapacityHeap[i];
      if (i < generation_slot_.size()) {
        generation_slot_[i].assign(kCapacityHeap[i], 0);
      }
    }
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         kCapacityHeap[0] + kCapacityHeap[1] + kCapacityHeap[2]},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kCapacityHeap[3]}};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info_pool.maxSets = 4;
    info_pool.poolSizeCount = 2;
    info_pool.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &pool_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve.descriptorPool = pool_;
    reserve.descriptorSetCount = 4;
    reserve.pSetLayouts = layouts_.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve, sets_.data()) != VK_SUCCESS) {
      return false;
    }
    // --- Set 4, the constants through dynamic UBOs ------------------------------------------------------
    // Always created: the shaders of the current library use it statically even with the bit off (it is
    // then bound with offsets 0). With an older library it is unnecessary and harmless. No
    // UPDATE_AFTER_BIND: dynamic descriptors do not support it. CreateUpload runs first, so the buffers
    // already exist.
    use_ubo_ = REXCVAR_GET(bo2zm_native_constants_ubo);
    cache_between_frames_ = REXCVAR_GET(bo2zm_native_texture_cache_across_frames);
    REXLOG_INFO("[native] C6: texture caches across frames (bo2zm_native_texture_cache_across_frames) = {}",
                cache_between_frames_ ? "SI" : "no");
    mipmaps_ = REXCVAR_GET(bo2zm_native_mipmaps);
    REXLOG_INFO("[native] C3: texture mip levels (bo2zm_native_mipmaps) = {}", mipmaps_ ? "SI" : "no");
    textures_mb_max_ = REXCVAR_GET(bo2zm_native_textures_mb_max);
    test_no_memory_every_ = REXCVAR_GET(bo2zm_native_test_out_of_memory_every);
    if (test_no_memory_every_ > 0) {
      REXLOG_WARN("[native] C3: TEST on: running out of memory will be faked every {} texture allocations",
                  test_no_memory_every_);
    }
    REXLOG_INFO("[native] C3: texture cache limit (bo2zm_native_textures_mb_max) = {} MB{}", textures_mb_max_,
                textures_mb_max_ > 0 ? "" : " (no limit)");
    diag_mips_ = REXCVAR_GET(bo2zm_native_diag_mips);
    if (diag_mips_) {
      REXLOG_INFO("[native] C3: mip diagnostic (bo2zm_native_diag_mips) = YES");
    }
    alignment_ubo_ = std::max<VkDeviceSize>(16, vulkan_device_->properties().minUniformBufferOffsetAlignment);
    alternate_ubo_s_ = REXCVAR_GET(bo2zm_native_constants_ubo_alternate_s);
    REXLOG_INFO("[native] C6: constants by dynamic UBO (bo2zm_native_constants_ubo) = {}; alternate every {} s; "
                "alignment {} bytes",
                use_ubo_ ? "SI" : "no", alternate_ubo_s_, alignment_ubo_);
    std::array<VkDescriptorSetLayoutBinding, 3> bindings_ubo{};
    for (uint32_t b = 0; b < 3; ++b) {
      bindings_ubo[b].binding = b;
      bindings_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      bindings_ubo[b].descriptorCount = 1;
      bindings_ubo[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info_ubo{};
    info_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_ubo.bindingCount = 3;
    info_ubo.pBindings = bindings_ubo.data();
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_ubo, nullptr, &layout_ubo_) != VK_SUCCESS) {
      return false;
    }
    const VkDescriptorPoolSize size_ubo{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * uint32_t(sets_ubo_.size())};
    VkDescriptorPoolCreateInfo info_pool_ubo{};
    info_pool_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_ubo.maxSets = uint32_t(sets_ubo_.size());
    info_pool_ubo.poolSizeCount = 1;
    info_pool_ubo.pPoolSizes = &size_ubo;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_ubo, nullptr, &pool_ubo_) != VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorSetLayout, kSlotsOfJob> layouts_ubo;
    layouts_ubo.fill(layout_ubo_);
    VkDescriptorSetAllocateInfo reserve_ubo{};
    reserve_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve_ubo.descriptorPool = pool_ubo_;
    reserve_ubo.descriptorSetCount = uint32_t(sets_ubo_.size());
    reserve_ubo.pSetLayouts = layouts_ubo.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve_ubo, sets_ubo_.data()) != VK_SUCCESS) {
      return false;
    }
    for (size_t slot = 0; slot < sets_ubo_.size(); ++slot) {
      const VkDescriptorBufferInfo blocks[3] = {{uploads_[slot].buffer, 0, kUboBytesVs},
                                                 {uploads_[slot].buffer, 0, kUboBytesPs},
                                                 {shared_separate_ ? shared_bufs_[slot].buffer
                                                                      : uploads_[slot].buffer,
                                                  0, kUboBytesShared}};
      std::array<VkWriteDescriptorSet, 3> writes_ubo{};
      for (uint32_t b = 0; b < 3; ++b) {
        writes_ubo[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes_ubo[b].dstSet = sets_ubo_[slot];
        writes_ubo[b].dstBinding = b;
        writes_ubo[b].descriptorCount = 1;
        writes_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes_ubo[b].pBufferInfo = &blocks[b];
      }
      dfn_.vkUpdateDescriptorSets(device_, 3, writes_ubo.data(), 0, nullptr);
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    24};
    VkPipelineLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const std::array<VkDescriptorSetLayout, 5> layouts_pipeline = {layouts_[0], layouts_[1], layouts_[2],
                                                                    layouts_[3], layout_ubo_};
    info_layout.setLayoutCount = 5;
    info_layout.pSetLayouts = layouts_pipeline.data();
    info_layout.pushConstantRangeCount = 1;
    info_layout.pPushConstantRanges = &range;
    return dfn_.vkCreatePipelineLayout(device_, &info_layout, nullptr, &layout_pipeline_) ==
           VK_SUCCESS;
  }

  // Slot 0 of each heap: a transparent black texture and a basic sampler, as the emulation does for an
  // invalid fetch constant.
  bool CreateEmpty() {
    const std::array<std::pair<VkImageViewType, uint32_t>, 3> types = {
        {{VK_IMAGE_VIEW_TYPE_2D, 1}, {VK_IMAGE_VIEW_TYPE_3D, 1}, {VK_IMAGE_VIEW_TYPE_CUBE, 6}}};
    for (uint32_t i = 0; i < 3; ++i) {
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = i == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      info.flags = i == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.extent = {1, 1, 1};
      info.mipLevels = 1;
      info.arrayLayers = types[i].second;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
              empty_[i].image, empty_[i].guest_memory)) {
        return false;
      }
      VkImageViewCreateInfo view{};
      view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view.image = empty_[i].image;
      view.viewType = types[i].first;
      view.format = VK_FORMAT_R8G8B8A8_UNORM;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, types[i].second};
      if (dfn_.vkCreateImageView(device_, &view, nullptr, &empty_[i].view) != VK_SUCCESS) {
        return false;
      }
      empty_[i].width = empty_[i].height = 1;
      empty_[i].format = VK_FORMAT_R8G8B8A8_UNORM;
      WriteImage(i, 0, empty_[i].view);
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    if (dfn_.vkCreateSampler(device_, &sampler, nullptr, &sampler_empty_) != VK_SUCCESS) {
      return false;
    }
    WriteSampler(0, sampler_empty_);
    return true;
  }

  // The empty ones are initialized on the first submission: GENERAL and cleared to zero.
  bool PrepareEmpty() {
    if (empty_prepared_) {
      return true;
    }
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload) {
      return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t layers = i == 2 ? 6 : 1;
      Barrier(upload, empty_[i].image, layers);
      VkClearColorValue cero{};
      if (REXCVAR_GET(bo2zm_native_diag_gray_empties)) {
        cero.float32[0] = cero.float32[1] = cero.float32[2] = 0.5f;
        cero.float32[3] = 1.0f;
      }
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
      gpu_.ClearColorImage(upload, empty_[i].image, VK_IMAGE_LAYOUT_GENERAL, &cero, 1,
                           &range);
      empty_[i].prepared = true;
    }
    empty_prepared_ = true;
    return true;
  }

  void Barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, layers};  // and mips
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    gpu_.Barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                 &barrier);
  }

  void WriteImage(uint32_t heap, uint32_t slot, VkImageView view) {
    VkDescriptorImageInfo image{};
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[heap];
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  void WriteSampler(uint32_t slot, VkSampler sampler) {
    VkDescriptorImageInfo image{};
    image.sampler = sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[3];
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  uint32_t ReserveSlot(uint32_t heap) {
    auto& m = heaps_[heap];
    if (!m.free.empty()) {
      const uint32_t slot = m.free.back();
      m.free.pop_back();
      return slot;
    }
    return m.next < m.capacity ? m.next++ : 0;
  }

  // The draw's vertex input, recomputed only when the patched VS changes.
  const VertexInput* InputOf(const DrawRequest& p) {
    if (input_generation_ == p.generation_vs && input_vs_ == p.vs) {
      if (input_valid_) {
        ++inputs_reused_;
        return input_current_;
      }
      // The same input that already failed: no recomputation and no warning.
      ++rejected_;
      ++causes_[input_cause_];
      return nullptr;
    }
    input_generation_ = p.generation_vs;
    input_vs_ = p.vs;
    input_valid_ = false;
    /*
     * bo2zm_native_input_front: the ring gives each distinct VS microcode content its own
     * generation (LoadShaderCached), so (generation, VS) names everything KeyInput reads: a small direct-mapped table
     * by that pair finds the cache element without KeyInput's gather of the fetch words, its XXH3 (up to 768 bytes)
     * and the map's walk. Elements of inputs_cache_ are never erased, so the pointers stay valid. Self-checking
     * guard: the first kInputFrontChecks hits, then 1 in 1,024, also take the usual path and compare; a difference
     * turns the table off for the rest of the run with a MISMATCH line.
     */
    InputFront& front = inputs_front_[(p.generation_vs * 0x9E3779B97F4A7C15ull) >> (64 - kInputFrontBits)];
    if (inputs_front_on_ && front.input && front.generation == p.generation_vs && front.vs == p.vs) {
      const bool check = inputs_front_checks_ < kInputFrontChecks || (++inputs_front_sampled_ & 1023) == 0;
      if (!check) {
        input_current_ = front.input;
        input_valid_ = true;
        input_cause_ = 0;
        ++inputs_cache_hits_;
        ++inputs_front_hits_;
        return input_current_;
      }
      ++inputs_front_checks_;
      const uint64_t key_check = KeyInput(p);
      const auto it = key_check ? inputs_cache_.find(key_check) : inputs_cache_.end();
      if (it == inputs_cache_.end() || &it->second != front.input) {
        inputs_front_off_ = true;
        inputs_front_on_ = false;
        REXLOG_WARN("[native] C6 vertex inputs by VS generation: MISMATCH after {} checks (VS n{}, generation {}): "
                    "off for the session",
                    inputs_front_checks_, p.vs->number, p.generation_vs);
      }
    }
    // Cache: the same VS with the same patched fetches gives the same input.
    const uint64_t key_input = KeyInput(p);
    if (key_input) {
      if (const auto it = inputs_cache_.find(key_input); it != inputs_cache_.end()) {
        input_current_ = &it->second;
        input_valid_ = true;
        input_cause_ = 0;
        ++inputs_cache_hits_;
        front = {p.generation_vs, p.vs, &it->second};
        return input_current_;
      }
    }
    input_ = VertexInput{};
    const uint64_t rejected_before = rejected_;
    const auto before_input = std::chrono::steady_clock::now();
    const VertexInput* input = ComputeInput(p);
    ns_inputs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - before_input)
                                 .count());
    ++inputs_computed_;
    input_cause_ = rejected_ != rejected_before ? last_cause_ : 0;
    input_current_ = &input_;
    if (input && key_input && inputs_cache_.size() < 4096) {
      const auto [it, added] = inputs_cache_.emplace(key_input, input_);
      front = {p.generation_vs, p.vs, &it->second};
    }
    return input;
  }

  // Key of the input cache: everything ComputeInput reads from the elements and from both microcodes
  // (instruction, usage and index; original and patched words of each fetch), seeded with the VS. 0 = do
  // not cache.
  uint64_t KeyInput(const DrawRequest& p) const {
    const ShaderInput& vs = *p.vs;
    const auto& patched = p.vs_microcode;
    if (patched.size() != vs.microcode.size() || vs.elements.size() > 32) {
      return 0;
    }
    std::array<uint32_t, 32 * 6> words;
    size_t n = 0;
    for (const VertexElement& element : vs.elements) {
      const size_t i = size_t(element.instruction) * 3;
      if (i + 2 >= patched.size()) {
        return 0;
      }
      words[n++] = (uint32_t(element.instruction) << 16) | (uint32_t(element.use) << 8) |
                      element.index_use;
      words[n++] = vs.microcode[i];
      words[n++] = vs.microcode[i + 1];
      words[n++] = patched[i];
      words[n++] = patched[i + 1];
      words[n++] = patched[i + 2];
    }
    const uint64_t key =
        XXH3_64bits_withSeed(words.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)));
    return key ? key : 1;
  }

  const VertexInput* ComputeInput(const DrawRequest& p) {
    const ShaderInput& vs = *p.vs;
    input_.remaps.fill(kRemapIdentity);
    const auto& patched = p.vs_microcode;
    if (patched.size() != vs.microcode.size()) {
      Reject(20, "the ring's VS does not have the container's length");
      return nullptr;
    }
    uint32_t locations_used = 0;
    for (const VertexElement& element : vs.elements) {
      const uint32_t reg_index = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
      const uint32_t original = vs.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF;
      size_t q = SIZE_MAX;
      for (const VertexElement& other : vs.elements) {
        const size_t i = size_t(other.instruction) * 3;
        if (((patched[i] >> 12) & 0x3F) != reg_index || (patched[i] & 0x1F) != 0) {
          continue;
        }
        if (q == SIZE_MAX) {
          q = i;
        }
        if (MaskWritten(patched[i + 1] & 0xFFF) == MaskWritten(original)) {
          q = i;  // the one that writes the same components
          break;
        }
      }
      if (q == SIZE_MAX) {
        if (warned_vs_.size() < 16 && warned_vs_.insert(vs.number).second) {
          std::string detail;
          for (const VertexElement& other : vs.elements) {
            const size_t i = size_t(other.instruction) * 3;
            detail += fmt::format(" {}{}@{}: original r{} op{}, patched r{} op{} {:08X} {:08X} {:08X};",
                                   NameUse(other.use), other.index_use, other.instruction,
                                   (vs.microcode[i] >> 12) & 0x3F, vs.microcode[i] & 0x1F,
                                   (patched[i] >> 12) & 0x3F, patched[i] & 0x1F, patched[i],
                                   patched[i + 1], patched[i + 2]);
          }
          REXLOG_WARN("[native] C6 diag: VS n{} without a fetch for {}{} (register r{}):{}", vs.number,
                      NameUse(element.use), element.index_use, reg_index, detail);
        }
        Reject(21, "vertex fetch not found in the patched VS");
        return nullptr;
      }
      const uint32_t d0 = patched[q], d1 = patched[q + 1], d2 = patched[q + 2];
      const uint32_t slot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
      const uint32_t format = (d1 >> 16) & 0x3F;
      const uint32_t stride = (d2 & 0xFF) * 4;
      const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
      const int32_t location = UseLocation(element.use, element.index_use);
      if (location < 0 || ((locations_used >> location) & 0x1)) {
        Warn(22, "vertex element without a free location in the shader: skipped");
        continue;
      }
      const uint32_t code = CodeRemap(original, d1 & 0xFFF);
      if (code != kRemapIdentity && warnings_swizzle_ < 24) {
        ++warnings_swizzle_;
        REXLOG_INFO("[native] C6: VS n{} {}{} (format {}): original swizzle {:03X}, patched {:03X}, remap {:03X}",
                    vs.number, NameUse(element.use), element.index_use, format, original,
                    d1 & 0xFFF, code);
      }
      bool r11g11b10 = false;
      const VkFormat vk = FormatAttribute(format, InputWhole(element.use), (d1 >> 12) & 0x1,
                                          (d1 >> 13) & 0x1, false, r11g11b10);
      if (vk == VK_FORMAT_UNDEFINED) {
        Reject(300 + format, "vertex format not supported yet");
        return nullptr;
      }
      if (r11g11b10) {
        input_.specialization |= 0x1;
      }
      uint32_t binding = 0;
      while (binding < input_.bindings.size() && input_.bindings[binding].slot != slot) {
        ++binding;
      }
      if (binding == input_.bindings.size()) {
        input_.bindings.push_back({slot, stride});
      } else if (!input_.bindings[binding].stride) {
        input_.bindings[binding].stride = stride;
      }
      if (offset < 0) {
        Reject(24, "negative vertex offset");
        return nullptr;
      }
      input_.attributes.push_back({uint32_t(location), binding, vk, uint32_t(offset)});
      input_.remaps[location] = code;
      locations_used |= uint32_t(1) << location;
    }
    for (const VertexBinding& binding : input_.bindings) {
      if (!binding.stride) {
        Reject(25, "vertex stream without a stride");
        return nullptr;
      }
    }
    uint64_t hash = XXH3_64bits(input_.attributes.data(),
                                  input_.attributes.size() * sizeof(VertexAttribute));
    hash = XXH3_64bits_withSeed(input_.bindings.data(),
                                  input_.bindings.size() * sizeof(VertexBinding), hash);
    input_.hash = hash ^ input_.specialization;
    input_valid_ = true;
    return &input_;
  }

  // Diagnostics (bo2zm_native_dump_texture): header "BO2T", the 6 fetch words, then the base level's
  // bytes (`bytes`: rows and columns aligned to 32 blocks) and 1 MB from the mip address (if there is one).
  void DumpTextureBo2zm(const uint32_t* f, uint64_t address, uint64_t bytes, uint64_t dir_mips) {
    // "mips": the first DXT1 texture of 256 texels or more with a mip chain.
    static const uint64_t goal = [] {
      const std::string& text = REXCVAR_GET(bo2zm_native_dump_texture);
      return text.empty() ? UINT64_MAX : text == "mips" ? UINT64_MAX - 1 : std::strtoull(text.c_str(), nullptr, 16);
    }();
    const bool first_with_mips = goal == UINT64_MAX - 1 && dir_mips && (f[1] & 0x3F) == 18 &&
                                  (f[2] & 0x1FFF) >= 255 && ((f[2] >> 13) & 0x1FFF) >= 255;
    if ((address != goal && !first_with_mips) || dump_bo2zm_ || address + bytes > kMemoryPhysical) {
      return;
    }
    static const auto start = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds(REXCVAR_GET(bo2zm_native_dump_texture_after_s))) {
      return;
    }
    dump_bo2zm_ = true;
    const std::filesystem::path path =
        rex::filesystem::GetExecutableFolder() / fmt::format("dump_{:08X}.bin", uint32_t(address));
    std::ofstream output(path, std::ios::binary);
    output.write("BO2T", 4);
    output.write(reinterpret_cast<const char*>(f), sizeof(uint32_t) * 6);
    const uint64_t mips = dir_mips && dir_mips + (1u << 20) <= kMemoryPhysical ? (1u << 20) : 0;
    const uint64_t sizes[2] = {bytes, mips};
    output.write(reinterpret_cast<const char*>(sizes), sizeof(sizes));
    output.write(reinterpret_cast<const char*>(memory_->TranslatePhysical(uint32_t(address))), std::streamsize(bytes));
    if (mips) {
      output.write(reinterpret_cast<const char*>(memory_->TranslatePhysical(uint32_t(dir_mips))), std::streamsize(mips));
    }
    g_bo2zm_dump_texture.store((address << 32) | ((dir_mips ? dir_mips + mips : address + bytes) - address),
                                  std::memory_order_relaxed);
    REXLOG_INFO("[native] BO2: texture {:08X} dumped to {} ({} bytes; fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                uint32_t(address), path.string(), bytes, f[0], f[1], f[2], f[3], f[4], f[5]);
  }
  bool dump_bo2zm_ = false;

  // Texture of a fetch constant: heap slot and, if it has to be uploaded, it is left in textures_a_upload_
  // with its data already prepared. Base level of 2D textures and cubemaps.
  /*
   * bo2zm_native_texture_by_writes. A stable texture (unchanged over its last checks:
   * interval 8 or more, so never a video) whose recheck is due is taken as unchanged without reading its bytes while
   * no page its check reads has a write stamp from that check's epoch on (bo2zm_page_writes.h: the game's copy and
   * fill routines, the skinning hooks and memexport stamp what they write). The guard: the first kStampGuardFirst,
   * then 1 in kStampGuardEvery of each texture's, are checked as usual anyway; a change found then had no stamp
   * (TextureChangedWithoutStamp), and its pages count as always written from then on.
   * bo2zm_native_texture_by_writes_young: from interval 2 instead of 8, and 1 in kStampGuardEveryYoung of the skips
   * below interval 8 are guarded.
   */
  static constexpr uint64_t kStampGuardFirst = 4000;
  static constexpr uint16_t kStampGuardEvery = 16;
  static constexpr uint16_t kStampGuardEveryYoung = 4;

  bool TextureUnwritten(CachedTexture& texture, uint64_t key, uint64_t& valid_until) {
    texture.guard_stamps = false;
    const bool young = texture.interval < 8;
    if (!tex_by_writes_active_ || !texture.image.prepared || texture.check_epoch == 0 ||
        texture.interval < (tex_young_active_ ? 2u : 8u) || texture.hash_job != 0) {
      return false;
    }
    if (!bo2zm::page_writes::Unchanged(texture.stamp_base, texture.stamp_base_bytes, texture.check_epoch) ||
        !bo2zm::page_writes::Unchanged(texture.stamp_mips, texture.stamp_mips_bytes, texture.check_epoch)) {
      ++tex_stamp_written_;
      return false;
    }
    if (++tex_stamp_seen_ <= kStampGuardFirst ||
        ++texture.stamp_skips >= (young ? kStampGuardEveryYoung : kStampGuardEvery)) {
      texture.stamp_skips = 0;
      texture.guard_stamps = true;
      ++tex_stamp_guarded_;
      return false;
    }
    ++tex_stamp_skipped_;
    tex_stamp_skipped_young_ += young ? 1 : 0;
    texture.frame = frame_;  // in use (eviction) and checked this frame
    texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
    texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
    valid_until = texture.next - 1;
    return true;
  }

  // A guard check found new bytes where the stamps said nothing was written: a writer that does not stamp.
  void TextureChangedWithoutStamp(CachedTexture& texture, uint32_t width, uint32_t height, uint32_t format) {
    texture.guard_stamps = false;
    ++tex_unstamped_interval_;
    tex_unstamped_young_ += texture.interval < 8 ? 1 : 0;
    if (++tex_unstamped_ <= 16) {
      REXLOG_WARN("[native] C3 texture by writes: {:08X}+{} (mips {:08X}+{}) {}x{} format {} changed without a write "
                  "stamp (a writer that does not stamp); its pages now count as written every frame",
                  texture.stamp_base, texture.stamp_base_bytes, texture.stamp_mips, texture.stamp_mips_bytes, width,
                  height, format);
    }
    bo2zm::page_writes::MarkVolatile(texture.stamp_base, texture.stamp_base_bytes);
    bo2zm::page_writes::MarkVolatile(texture.stamp_mips, texture.stamp_mips_bytes);
  }

  void ReportTextureStamps() {
    if (tex_by_writes_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < tex_stamps_next_) {
      return;
    }
    const bool first = tex_stamps_next_ == std::chrono::steady_clock::time_point{};
    tex_stamps_next_ = now + std::chrono::seconds(10);
    if (!first) {
      BO2ZM_REPORT_RING("[native] C3 texture rechecks by write stamps, last 10 s: {} skipped (no stamp since the last "
                        "check; {} of them young, interval 2-4), {} checked anyway (the guard), {} checked (a stamp "
                        "since); {} changed without a stamp ({} since start, {} of them young), their pages now count "
                        "as always written",
                        tex_stamp_skipped_, tex_stamp_skipped_young_, tex_stamp_guarded_, tex_stamp_written_,
                        tex_unstamped_interval_, tex_unstamped_, tex_unstamped_young_);
    }
    tex_stamp_skipped_ = tex_stamp_skipped_young_ = tex_stamp_guarded_ = tex_stamp_written_ =
        tex_unstamped_interval_ = 0;
  }

  /*
   * bo2zm_native_texture_reuse. In Town ~120 textures a second are new, and each costs 150-370 us of
   * the ring thread to create, read, untile and upload. The game brings textures back at new addresses (probably
   * its image streaming), and the content measurement
   * (bo2zm_native_diag_reuse) found ~30 % of the new ones with the same guest bytes and shape as a live texture not
   * used for more than 120 frames. Such a new texture takes that cold one's image: its views and descriptor slots go
   * with it (views_ is keyed by image) and the cold one leaves the cache, so nothing is created, read or uploaded.
   * The image holds what was read from the bytes its raw hash covers (a texture is uploaded from the same check
   * that sets that hash), so equal raw hashes and shapes mean equal host data. Only a cold one is taken, so a
   * texture in use never loses its image (its fetch caches expired long ago: a texture is checked at least every
   * 39 frames while in use); one whose upload, bind or hash is still pending is not taken.
   */
  bool TextureReuse() {
    if (texreuse_ < 0) {
      texreuse_ab_ = REXCVAR_GET(bo2zm_native_texture_reuse_ab);
      texreuse_ = REXCVAR_GET(bo2zm_native_texture_reuse) || texreuse_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C3: new textures take a cold texture's image with the same content "
                  "(bo2zm_native_texture_reuse) = {}{}",
                  texreuse_ ? "YES" : "no", texreuse_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    return texreuse_ > 0;
  }

  // The raw hash a texture's check takes: its base bytes, then its mips with that as the seed.
  uint64_t HashRaw(uint64_t start_raw, uint64_t extension, uint64_t dir_mips, uint64_t extension_mips) {
    uint64_t hash = XXH3_64bits(memory_->TranslatePhysical(uint32_t(start_raw)), size_t(extension));
    if (extension_mips) {
      hash = XXH3_64bits_withSeed(memory_->TranslatePhysical(uint32_t(dir_mips)), size_t(extension_mips), hash);
    }
    return hash;
  }

  bool ReuseByContent(CachedTexture& texture, uint64_t key, uint64_t shape_content, VkFormat format,
                      uint32_t width_host, uint32_t height_host, uint32_t levels, uint32_t layers, uint64_t hash_raw,
                      uint32_t address) {
    const uint64_t content =
        KeyContentOf(shape_content, format, levels, width_host, height_host, layers, hash_raw);
    uint64_t taken_key = 0;
    uint32_t looked = 0;
    const auto [since, until] = live_per_content_.equal_range(content);
    for (auto it = since; it != until && looked < kReuseLookMax; ++it, ++looked) {
      if (it->second == key) {
        continue;
      }
      const auto other = textures_.find(it->second);
      if (other == textures_.end()) {
        continue;
      }
      const CachedTexture& t = other->second;
      if (t.key_content == content && t.image.image != VK_NULL_HANDLE && t.image.prepared && !t.needs_upload &&
          !t.in_flight && !t.hash_job && &t != comparison_texture_ && t.frame != UINT64_MAX &&
          t.frame + kFramesNoUseForRelease < frame_) {
        taken_key = it->second;
        break;
      }
    }
    if (!taken_key) {
      ++texreuse_missed_;
      return false;
    }
    CachedTexture& old = textures_.find(taken_key)->second;
    RemoveContentTexture(old, taken_key);
    texture.image = old.image;  // its views and slots come with it; bytes_textures_ already counts it
    old.image = NativeImage{};
    texture.layers = old.layers;
    texture.depth3d = old.depth3d;
    texture.levels = old.levels;
    texture.offset_level = old.offset_level;
    texture.bytes = old.bytes;
    texture.hash = old.hash;
    texture.hash_raw = hash_raw;
    texture.shape_content = shape_content;
    texture.address = address;
    texture.content_per_measure = false;  // not a new image for the measurement
    textures_.erase(taken_key);
    AnnotateContentTexture(texture, key);  // filed under the same content
    // Just checked against the bytes the image holds: rechecked in 8 frames, then as a stable texture.
    texture.frame = frame_;
    texture.interval = 8;
    texture.next = frame_ + 8;
    ++texreuse_taken_;
    ++texreuse_taken_total_;
    texreuse_bytes_ += texture.bytes;
    return true;
  }

  void ReportTextureReuse() {
    if (texreuse_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < texreuse_next_) {
      return;
    }
    const bool first = texreuse_next_ == std::chrono::steady_clock::time_point{};
    texreuse_next_ = now + std::chrono::seconds(10);
    if (!first) {
      BO2ZM_REPORT_RING("[native] C3 new textures by content, last 10 s: {} took a cold texture's image ({:.1f} MB not "
                        "created, read or uploaded; {} since start), {} looked up without a match",
                        texreuse_taken_, double(texreuse_bytes_) / 1048576.0, texreuse_taken_total_,
                        texreuse_missed_);
    }
    texreuse_taken_ = texreuse_bytes_ = texreuse_missed_ = 0;
  }

  void PrepareTexture(const uint32_t* f, uint32_t& slot, uint32_t& heap,
                       VkDeviceSize& bytes_upload, bool& sampling_point, uint64_t& valid_until,
                       uint32_t& width_host_out, uint32_t& height_host_out) {
    slot = 0;
    heap = 0;
    width_host_out = 0;  // 0 = unknown (1/size is not written)
    height_host_out = 0;
    sampling_point = false;
    valid_until = frame_;  // by default, this frame only
    prepare_outcome_ = 0;  // which way it went, for the C6 substages (PrepareOutcome)
    prepare_new_ = false;
    if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
      return;
    }
    const uint32_t dimension = (f[5] >> 9) & 0x3;
    const bool cube = dimension == uint32_t(xenos::DataDimension::kCube);
    const bool volume = dimension == uint32_t(xenos::DataDimension::k3D);
    if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cube && !volume) {
      Warn(30, "1D textures not supported yet: an empty one is used");
      return;
    }
    heap = cube ? 2 : volume ? 1 : 0;
    const uint32_t layers = cube ? 6 : 1;
    if ((f[0] >> 2) & 0xFF) {
      Warn(31, "signed or gamma textures: read as unsigned");
    }
    const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
    const uint32_t base = (f[1] >> 12) << 12;
    if (!cube && !volume) {
      if (const NativeImage* resolved = context_->TextureResolved(base & 0x1FFFFFFF)) {
        // Without this the shadows flicker.
        // With bo2zm_native_inv_tex_size the shader takes 1/size from the shared constants. If this path
        // returned without reporting the size, 0 would be written and the tfetch offsets (the taps of the
        // shadow map's PCF filter) would all land on the same texel: the filtering disappears and the edge
        // shimmers as the camera moves. Resolved textures report their size like any other.
        width_host_out = resolved->width;
        height_host_out = resolved->height;
        // One trace per size, to check in the log that the 1/size constant of resolved textures is not 0
        // (which would make the shadows flicker).
        if (warnings_invsize_resolved_.insert(uint64_t(resolved->width) << 32 | resolved->height).second) {
          REXLOG_INFO("[native] C3: resolved texture {}x{}: 1/size = {:.6f}, {:.6f}",
                      resolved->width, resolved->height, 1.0f / float(resolved->width),
                      1.0f / float(resolved->height));
        }
        if (IsDepth(resolved->format)) {
          // Depth copy (k_24_8): depth comes out in R and the fetch constant's swizzle distributes it. No
          // filtering: it is read as is.
          slot = SlotView(resolved->image, resolved->format, swizzle, kSwizzleRRRR);
          sampling_point = true;
          // Valid for this frame only, not UINT64_MAX.
          // A resolved texture is a render target the game rewrites every frame. Saying its descriptor slot is
          // valid "until the generation changes" would mean that, with the CPU two frames ahead (3 work slots),
          // an old view is reused and the image smears; the cross-frame cache
          // (bo2zm_native_texture_cache_across_frames) depends on this. The other paths are tied to texture.next;
          // these two are the only ones without a horizon of their own.
          valid_until = frame_;
          return;
        }
        // The emulation writes the copy to memory with copy_dest_swap and loads it as a texture: the fetch
        // constant's swizzle (ZYXW for 8888) undoes that swap. Here the copy is image to image and does not
        // swap channels, so the swap goes into the host channels (without this, Mia came out blue).
        slot = SlotView(resolved->image, resolved->format, swizzle,
                             resolved->swap_rb ? kSwizzleBGRA : kSwizzleRGBA);
        valid_until = frame_;  // see the long comment on the depth copy
        return;
      }
    }
    // A 3D texture over the memory of a 2D resolve (BO2's color-grading table, sampled by the
    // tonemap). Without this it was read from guest memory, which C2's resolves never write: all zeros, and the
    // whole in-game image came out black.
    if (volume) {
      if (const NativeImage* resolved = context_->TextureResolved3D(
              base & 0x1FFFFFFF, f[1] & 0x3F, (f[2] & 0x7FF) + 1, ((f[2] >> 11) & 0x7FF) + 1,
              ((f[2] >> 22) & 0x3FF) + 1, ((f[0] >> 22) & 0x1FF) << 5, (f[0] >> 31) & 0x1)) {
        slot = SlotView(resolved->image, resolved->format, swizzle,
                             resolved->swap_rb ? kSwizzleBGRA : kSwizzleRGBA, heap);
        width_host_out = resolved->width;
        height_host_out = resolved->height;
        valid_until = frame_;
        return;
      }
    }
    const uint32_t format = f[1] & 0x3F;
    HostTextureFormat tf;
    if (!FormatTextureOf(format, tf)) {
      Warn(400 + format, "texture format not supported yet: an empty one is used");
      return;
    }
    // size_2d: 13 + 13 bits; size_3d: 11 + 11 + 10 bits (xenos.h:1222-1233).
    const uint32_t width = volume ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
    const uint32_t height = volume ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
    const uint32_t depth3d = volume ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
    if (volume && tf.block > 1) {
      Warn(38, "compressed 3D texture: an empty one is used");
      return;
    }
    const uint32_t blocks_x = (width + tf.block - 1) / tf.block;
    const uint32_t blocks_y = (height + tf.block - 1) / tf.block;
    const uint32_t pitch_blocks = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.block, 1);
    // The faces of a cubemap are consecutive: each takes its base level with rows and columns aligned to
    // 32 blocks and the total to 4 KB (GetGuestTextureLayout, pipeline/texture/util.cpp:311-337).
    const uint64_t stride_face =
        (uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes *
             ((blocks_y + 31) & ~uint32_t(31)) +
         4095) &
        ~uint64_t(4095);
    const uint32_t keys[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
    const uint64_t key = XXH3_64bits(keys, sizeof(keys));

    // Mip levels with the rules of GetSubresourcesFromFetchConstant (pipeline/texture/util.cpp:67-97): no
    // mip address means no mips; the maximum is clamped to the size; if the base is missing or the minimum
    // is above 0, the base is not read. 3D textures still use only the base.
    const bool tiled_texture = (f[0] >> 31) & 0x1;
    const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
    const uint64_t dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
    DumpTextureBo2zm(f, dir_base, stride_face, dir_mips);  // diagnostics
    const uint32_t level_packed =
        mipmaps_ && !volume && ((f[5] >> 11) & 0x1) ? LevelPacked(width, height) : UINT32_MAX;
    uint32_t level_max = 0;
    bool read_base = true;
    if (mipmaps_ && !volume && dir_mips != 0) {
      const uint32_t sz_max = Log2Floor(std::max(width, height));
      uint32_t level_min = std::min((f[4] >> 2) & 0xFu, sz_max);
      level_max = std::max(std::min((f[4] >> 6) & 0xFu, sz_max), level_min);
      if (level_max != 0) {
        if (dir_base == 0) {
          level_min = std::max(level_min, 1u);
        }
        read_base = level_min == 0;
      }
    }
    // Mip regions from dir_mips (GetGuestTextureLayout): each level stored with rows of 32 blocks computed
    // from the size rounded up to a power of 2 (and aligned to 256 bytes if the texture is linear) and
    // layers aligned to 4 KB; the levels of the packed tail share the region of the first of them.
    struct RegionMip {
      uint64_t displacement = 0;
      uint64_t row_bytes = 0;
      uint64_t stride = 0;
      uint32_t pitch_blocks = 0;
    };
    std::array<RegionMip, 16> regions{};
    uint64_t extension_mips = 0;
    if (level_max != 0) {
      const uint32_t last_level = level_packed == 0 ? 0 : std::min(level_max, level_packed);
      for (uint32_t s = level_packed == 0 ? 0 : 1; s <= last_level; ++s) {
        RegionMip& region = regions[s];
        const uint32_t row_texels = std::max(std::bit_ceil(width) >> s, 1u);
        const uint32_t rows_texels = std::max(std::bit_ceil(height) >> s, 1u);
        region.pitch_blocks = ((row_texels + tf.block - 1) / tf.block + 31) & ~31u;
        region.row_bytes = uint64_t(region.pitch_blocks) * tf.bytes;
        if (!tiled_texture) {
          region.row_bytes = (region.row_bytes + 255) & ~uint64_t(255);
        }
        const uint64_t rows_blocks = uint64_t(((rows_texels + tf.block - 1) / tf.block + 31) & ~31u);
        region.stride = (region.row_bytes * rows_blocks + 4095) & ~uint64_t(4095);
        region.displacement = extension_mips;
        extension_mips += region.stride * layers;
      }
      if (dir_mips + extension_mips > kMemoryPhysical) {
        level_max = 0;  // the mips would go past memory: base only
        extension_mips = 0;
        read_base = true;
      }
    }
    // With the packed tail starting at level 0 (short side of 16 texels or less), the base does not start
    // at its address either (VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl applies the same
    // offset to level 0).
    uint32_t base_ox = 0;
    uint32_t base_oy = 0;
    if (level_packed == 0) {
      OffsetPacked(width, height, tf.block, 0, base_ox, base_oy);
    }

    // The guest bytes a 2D or cube texture's check hashes: its base layers here, its mips at dir_mips.
    // computed before the cache lookup, so a new texture can be matched by content before its image is created
    // (ReuseByContent).
    uint64_t start_raw = 0;
    uint64_t extension = 0;
    if (!volume) {
      const uint32_t log2_block = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                                : tf.bytes >= 2 ? 1 : 0;
      // Extent of a layer: tiled, GetTiledAddressUpperBound2D (pipeline/texture/util.cpp:461-486); linear,
      // up to the last block. Includes the packed base offset (base_ox, base_oy; 0 for the rest).
      const uint32_t blocks_x_read = blocks_x + base_ox;
      const uint32_t blocks_y_read = blocks_y + base_oy;
      const uint64_t extent_layer =
          ((f[0] >> 31) & 0x1)
              ? uint64_t(std::max<int64_t>(
                    OffsetTiled2D(int32_t((blocks_x_read - 1) & ~31u),
                                            int32_t((blocks_y_read - 1) & ~31u), pitch_blocks,
                                            log2_block),
                    0)) +
                    (log2_block == 0   ? 0xA00u
                     : log2_block == 1 ? 0xC00u
                                        : (0x400u << log2_block))
              : uint64_t(std::max(pitch_blocks, blocks_x_read)) * tf.bytes * (blocks_y_read - 1) +
                    uint64_t(blocks_x_read) * tf.bytes;
      start_raw = uint64_t(base) & 0x1FFFFFFF;
      extension = (layers - 1) * stride_face + extent_layer;
    }

    if (cube && gpu_.loaded().copy_image) {
      // The game's dynamic cubemap: C2 resolves its faces one by one to those same addresses. They are
      // copied to the cubemap's layers in the upload command buffer, which runs before the work one, so they
      // lag one frame behind.
      std::array<const NativeImage*, 6> faces{};
      bool resolved_list = true;
      for (uint32_t c = 0; c < 6 && resolved_list; ++c) {
        faces[c] = context_->TextureResolved(
            uint32_t((uint64_t(base) + c * stride_face) & 0x1FFFFFFF));
        resolved_list = faces[c] && faces[c]->format == VK_FORMAT_R8G8B8A8_UNORM &&
                    faces[c]->width >= width && faces[c]->height >= height;
      }
      if (resolved_list) {
        const uint64_t key_resolved = key ^ 0x9E3779B97F4A7C15ull;
        CachedTexture& texture = textures_[key_resolved];
        if (texture.image.image == VK_NULL_HANDLE) {
          if (!CreateTexture(texture.image, VK_FORMAT_R8G8B8A8_UNORM, width, height, layers)) {
            textures_.erase(key_resolved);
            Warn(32, "could not create a texture");
            return;
          }
          texture.layers = layers;
          texture.bytes = uint64_t(width) * height * 4 * layers;
          bytes_textures_ += texture.bytes;
          REXLOG_INFO("[native] C3: cube {:08X} {}x{} with its faces resolved by C2 (fetch {:08X} {:08X} {:08X} {:08X} "
                      "{:08X} {:08X})",
                      base, width, height, f[0], f[1], f[2], f[3], f[4], f[5]);
        }
        if (texture.frame != frame_) {
          const VkCommandBuffer upload = context_->CommandsUpload();
          if (!upload) {
            return;
          }
          texture.frame = frame_;
          if (!texture.image.prepared) {
            Barrier(upload, texture.image.image, layers);
            texture.image.prepared = true;
          }
          for (uint32_t c = 0; c < 6; ++c) {
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c, 1};
            copy.extent = {width, height, 1};
            gpu_.CopyImage(upload, faces[c]->image, VK_IMAGE_LAYOUT_GENERAL, texture.image.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
          }
          // Reflection diagnostic: how many times the faces are copied to the cubemap per submission.
          ++cubes_refreshed_;
          const auto now = std::chrono::steady_clock::now();
          if (now - report_cubes_ >= std::chrono::seconds(10)) {
            // Like the other periodic lines, it goes to the report thread.
            BO2ZM_REPORT_RING("[native] C3 cubes with C2 faces: {} refreshes in {} jobs",
                                 cubes_refreshed_ - cubes_refreshed_previous_,
                                 frame_ - frame_report_cubes_);
            report_cubes_ = now;
            cubes_refreshed_previous_ = cubes_refreshed_;
            frame_report_cubes_ = frame_;
          }
        }
        slot = SlotView(texture.image.image, VK_FORMAT_R8G8B8A8_UNORM, swizzle,
                             faces[0]->swap_rb ? kSwizzleBGRA : kSwizzleRGBA, heap);
        // The cubemap with the resolved faces must also report its size, or its 1/size stays at 0 and the
        // tfetch offsets are lost (see the branch above).
        width_host_out = texture.image.width;
        height_host_out = texture.image.height;
        return;
      }
    }

    CachedTexture& texture = textures_[key];
    uint64_t hash_raw_known = 0;  // the raw hash ReuseByContent already took, if hash_known
    bool hash_known = false;
    auto t_new = std::chrono::steady_clock::time_point{};  // a new texture's parts (C6 substages 31-35)
    if (texture.image.image == VK_NULL_HANDLE) {
      if (time_) {
        t_new = std::chrono::steady_clock::now();
      }
      // BC in Vulkan: the size is in whole blocks.
      const uint32_t width_host = tf.block > 1 ? (width + 3) & ~uint32_t(3) : width;
      const uint32_t height_host = tf.block > 1 ? (height + 3) & ~uint32_t(3) : height;
      // With its mip levels (those that fit in the host size).
      const uint32_t levels = std::min(level_max + 1, Log2Floor(std::max(width_host, height_host)) + 1);
      // The shape without the address, for the content index (AnnotateContentTexture): the key's words without the
      // base and mip addresses and without bit 11 of f[1], which the sampler sets: format, byte order, tiling,
      // pitch, size, mip limits, dimension and packed tail; and whether the base address is 0 (its
      // base level is then not read).
      const uint32_t shape_words[5] = {keys[0], f[1] & 0x7FF, keys[2], keys[3],
                                       ((f[5] >> 9) & 0x7) | (dir_base == 0 ? 0x8u : 0u)};
      const uint64_t shape_content = XXH3_64bits(shape_words, sizeof(shape_words));
      // bo2zm_native_texture_reuse: the same bytes and shape as a cold texture: that one's image.
      if (texreuse_active_ && !volume && start_raw + extension <= kMemoryPhysical) {
        hash_raw_known = HashRaw(start_raw, extension, dir_mips, extension_mips);
        hash_known = true;
        const bool reused = ReuseByContent(texture, key, shape_content, tf.format, width_host, height_host, levels,
                                           layers, hash_raw_known, base);
        CutSubstage(31, t_new);
        if (reused) {
          texture.stamp_base = uint32_t(start_raw);
          texture.stamp_base_bytes = uint32_t(extension);
          texture.stamp_mips = uint32_t(dir_mips);
          texture.stamp_mips_bytes = uint32_t(extension_mips);
          texture.check_epoch =
              bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed) ? bo2zm::page_writes::Epoch() : 0;
          slot = SlotView(texture.image.image, tf.format, swizzle, tf.swizzle_host, heap);
          width_host_out = texture.image.width;
          height_host_out = texture.image.height;
          valid_until = texture.next - 1;
          prepare_outcome_ = 8;
          return;
        }
      }
      /*
       * The vkBindImageMemory goes to the bind thread if possible (bo2zm_native_texture_bind_thread) and,
       * otherwise, CreateTexture as usual. What the ring spends creating is timed, without its wait for the
       * thread (that is counted separately), for the [hitch] ring line and the 10 s report.
       */
      const auto before_create = std::chrono::steady_clock::now();
      const uint64_t wait_before_create = ns_wait_bindings_total_;
      measuring_creation_ = true;
      const bool created = CreateTextureInThread(texture, tf.format, width_host, height_host, layers, depth3d, levels) ||
                          CreateTexture(texture.image, tf.format, width_host, height_host, layers, depth3d, levels);
      measuring_creation_ = false;
      {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before_create)
                                         .count());
        const uint64_t wait = ns_wait_bindings_total_ - wait_before_create;
        const uint64_t own = ns > wait ? ns - wait : 0;
        bo2zm::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
        ns_create_report_ += own;
      }
      if (!created) {
        textures_.erase(key);
        Warn(32, "could not create a texture");
        return;
      }
      prepare_new_ = true;
      texture.layers = layers;
      texture.depth3d = depth3d;
      texture.levels = levels;
      levels_mip_uploaded_ += levels - 1;
      // Its shape, to file it in the content index once its first hash is known (AnnotateContentTexture): what
      // bo2zm_native_diag_reuse counts and bo2zm_native_texture_reuse looks up. If this image is being recreated,
      // the previous one leaves the index.
      if (ContentIndexed()) {
        RemoveContentTexture(texture, key);
        texture.shape_content = shape_content;
        texture.address = base;
        texture.content_per_measure = DiagReuse();
      }
      for (uint32_t n = 0; n < levels; ++n) {
        texture.bytes += uint64_t((std::max(width_host >> n, 1u) + tf.block - 1) / tf.block) *
                         ((std::max(height_host >> n, 1u) + tf.block - 1) / tf.block) * tf.bytes * layers *
                         (depth3d ? depth3d : 1);
      }
      bytes_textures_ += texture.bytes;
      // Diagnostic: why the cache grows on the console. A new texture at an address that already had another
      // points to world zones reloaded at the same place; with the same address, format and size but a
      // different key, to key fields that change (mip limits, byte order...).
      {
        ++textures_created_;
        bo2zm::waits::g_textures_created.fetch_add(1, std::memory_order_relaxed);
        const uint64_t shape_key = XXH3_64bits_withSeed(&base, sizeof(base), (uint64_t(format) << 40) ^
                                                                              (uint64_t(width) << 20) ^ height);
        // Fixed tables of kDiagSlots entries, by a hash of the address and of the shape, the newest
        // one taking a slot (so the counts are approximate). A map with an entry per address and shape ever seen
        // keeps growing in a long match (~0.3 MB a minute at round 15 and a 10 MB rehash after 10 minutes), and
        // emptying it every few minutes freezes the ring for 1-2 s (~100,000 frees in one frame).
        if (diag_shapes_.empty()) {
          diag_shapes_.resize(kDiagSlots);
          diag_addresses_.resize(kDiagSlots);
        }
        uint32_t& seen_base = diag_addresses_[(uint32_t(base) * 0x9E3779B1u) >> (32 - kDiagSlotBits)];
        if (seen_base == uint32_t(base) + 1) {
          ++created_in_address_view_;
        }
        seen_base = uint32_t(base) + 1;
        DiagShape& shape = diag_shapes_[size_t(shape_key >> (64 - kDiagSlotBits))];
        const bool new_shape = shape.shape != shape_key;
        if (!new_shape && shape.key != key) {
          ++created_same_shape_other_key_;
          shape.key = key;
          /*
           * In the main menu, ~100 textures per frame once came back with the same address, format and size and
           * a different key, and were created again every frame. This shows which key words change (the first
           * 40 times).
           */
          /*
           * Key guard. The same five words cannot produce a different key: if one does, the key was not computed
           * from what its words say (as when XXH3 read keys[4] before it was written; see docs/toolchain.md).
           */
          if (std::equal(std::begin(keys), std::end(keys), shape.words.begin())) {
            const uint64_t times = ++keys_incoherent_;
            if (times == 1 || times == 10 || times == 100 || times == 1000 || times % 10000 == 0) {
              REXLOG_ERROR("[native] C3 INCOHERENT texture key ({} times): {:08X} {}x{} format {} with the same five "
                           "words as the previous time and another key; the key does not come from its words",
                           times, base, width, height, format);
            }
          }
          if (warnings_other_key_ < 40) {
            ++warnings_other_key_;
            const auto& before = shape.words;
            REXLOG_INFO("[native] C3 same texture with another key: {:08X} {}x{} format {} | before {:08X} {:08X} "
                        "{:08X} {:02X} {:06X} | now {:08X} {:08X} {:08X} {:02X} {:06X} | changed {:08X} {:08X} {:08X} "
                        "{:02X} {:06X}",
                        base, width, height, format, before[0], before[1], before[2], before[3], before[4], keys[0],
                        keys[1], keys[2], keys[3], keys[4], before[0] ^ keys[0], before[1] ^ keys[1],
                        before[2] ^ keys[2], before[3] ^ keys[3], before[4] ^ keys[4]);
          }
          std::copy(std::begin(keys), std::end(keys), shape.words.begin());
        } else if (new_shape) {
          shape.shape = shape_key;
          shape.key = key;
          std::copy(std::begin(keys), std::end(keys), shape.words.begin());
        }
        // Every 1024. This line and the GPU memory line after it were the two longest stutters of the
        // "diagnostic dump" group (147.6 ms and 107.5 ms of frame time on their own, every 256). With 1024
        // they still appear once or twice per match, which is enough to see whether the duplicate key
        // counter spikes.
        if (textures_created_ % 1024 == 0) {
          REXLOG_INFO("[native] C3 diag cache: {} textures created ({} in the cache, {} MB): {} at an address that "
                      "already had another texture; {} with the same address, format and size as another but a "
                      "different key",
                      textures_created_, textures_.size(), bytes_textures_ >> 20, created_in_address_view_,
                      created_same_shape_other_key_);
          AnnotateGpuMemory();
        }
      }
      if (textures_.size() <= 48) {
        REXLOG_INFO("[native] C3: texture {:08X} {}x{} format {} {} order {} pitch {} swizzle {:03X} signs {:02X}{}; "
                    "levels {} (mips at {:08X}, packed from {})",
                    base, width, height, format, ((f[0] >> 31) & 0x1) ? "tiled" : "linear",
                    (f[1] >> 6) & 0x3, ((f[0] >> 22) & 0x1FF) << 5, swizzle, (f[0] >> 2) & 0xFF,
                    cube ? " (cube)" : volume ? " (3D)" : "", levels, dir_mips,
                    level_packed == UINT32_MAX ? std::string("none") : std::to_string(level_packed));
      }
      CutSubstage(32, t_new);
    }
    if (texture.staging_failed_frame == frame_) {
      slot = 0;
      valid_until = frame_;
      return;
    }
    slot = SlotView(texture.image.image, tf.format, swizzle, tf.swizzle_host, heap);
    width_host_out = texture.image.width;  // the host's, which is what the shader sees
    height_host_out = texture.image.height;
    if (texture.frame == frame_) {
      // Valid until the frame before the next check (if it changed now, only this frame)
      valid_until = std::max<uint64_t>(frame_, texture.next ? texture.next - 1 : 0);
      return;  // already checked this frame
    }
    // Textures that do not change are checked less and less often (down to every 32 frames); those that
    // change (videos) go back to being checked every frame.
    if (texture.image.prepared && frame_ < texture.next) {
      valid_until = texture.next - 1;
      return;
    }
    // A stable texture's due recheck, skipped while nothing wrote its pages since the last.
    if (TextureUnwritten(texture, key, valid_until)) {
      prepare_outcome_ = 1;
      return;
    }
    texture.frame = frame_;
    // If its hash thread job is still uncollected (a buffer-full submission happened in the middle of its
    // draw), its hash_raw is not valid yet: it is collected before checking, as if the ring had done
    // it.
    if (texture.hash_job == kJobHashPublished) {
      CollectHashes();
    }
    const uint64_t previous_hash_raw = texture.hash_raw;
    // What it costs from here to the end (check, untile, prepare the upload).
    struct TextureStopwatch {
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~TextureStopwatch() {
        bo2zm::waits::g_ns_textures.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                         .count()),
            std::memory_order_relaxed);
      }
    } stopwatch_texture;
    // 2D and cubemaps: before untiling, XXH3 of the guest bytes. If they have not changed there is nothing
    // to do (untiling and comparing every texture every frame took 62 of the 66 us of each menu draw).
    if (!volume) {
      if (start_raw + extension <= kMemoryPhysical) {
        /*
         * Per-frame check budget.
         * During gameplay stutters both game threads wait for room in the ring (20-50 ms) while the ring neither
         * stops nor waits for the GPU: the ring thread itself is stuck. During the stutters, that thread was in
         * PrepareTexture and in XXH3. Textures that arrive together (one zone) double their interval at the
         * same time (1, 2, 4... 32), so all of them are rechecked in the same frame. Here a stable texture
         * (interval 8 or more) whose check does not fit in the frame's budget is deferred a few frames, at
         * most kPostponementMax in a row. New textures, changing ones (videos) and just-uploaded ones are
         * never deferred.
         */
        /*
         * Sampled recheck of stable textures (bo2zm_native_hash_sampling).
         * Hashing a stable texture in full every 32-39 frames only to conclude, almost always, that it has not
         * changed costs 0.2-5.3 ms per frame of the ring thread in busy scenes. So every full check from
         * interval 4 on also computes the hash of a sample of the same memory (HashSample:
         * first and last 4 KB block and 1 in 8, of the base and the mips) and, if the full hash matches, stores
         * it. Rechecks of stable textures compute only the sample: if it matches, the texture is taken as
         * unchanged; otherwise the usual full path follows. Textures where the sample would read more than half
         * the bytes (those with few blocks) are always hashed in full.
         * Self-checking guard: the first kSamplesACheck rechecks of stable textures compute both hashes
         * and count disagreements (same sample, different full hash). After that, 1 in N rechecks of each
         * texture is still full (N = the cvar), so a change the sample misses is caught at most N rechecks
         * later. A single disagreement turns sampling off for the rest of the run, with a warning.
         */
        if (sampling_hashes_ < 0) {
          const int32_t every = REXCVAR_GET(bo2zm_native_hash_sampling);
          sampling_hashes_ = every >= 2 ? std::min<int32_t>(every, 64) : 0;
          REXLOG_INFO("[native] C3: recheck of stable textures by sampling (bo2zm_native_hash_sampling) = {}",
                      sampling_hashes_ ? fmt::format("YES, 1 in {} full; the first {} with both hashes",
                                                      sampling_hashes_, kSamplesACheck)
                                        : std::string("no, always the full hash"));
        }
        const uint64_t bytes_full = extension + extension_mips;
        const uint64_t sample_bytes = BytesSample(extension) + BytesSample(extension_mips);
        const bool with_sample = sampling_hashes_ > 0 && texture.image.prepared && texture.interval >= 4 &&
                                 sample_bytes * 2 <= bytes_full;
        const bool sample_useful = with_sample && texture.interval >= 8 && texture.sample_valid;
        const bool only_sample = sample_useful && samples_checked_ >= kSamplesACheck &&
                                  texture.samples_consecutive + 1u < uint32_t(sampling_hashes_);
        const uint64_t bytes_sample = with_sample ? sample_bytes : 0;
        // What will actually be read, which is what counts against the budget.
        const uint64_t bytes_hash = only_sample ? bytes_sample : bytes_full + bytes_sample;
        if (frame_hashes_ != frame_) {
          frame_hashes_ = frame_;
          bytes_hash_frame_ = 0;
        }
        if (budget_hashes_ < 0) {
          budget_hashes_ = std::max(REXCVAR_GET(bo2zm_native_hash_kb_per_frame), 0);
          REXLOG_INFO("[native] C3: texture check budget = {} KB per frame{}",
                      budget_hashes_, budget_hashes_ ? "" : " (no limit)");
        }
        if (budget_hashes_ && texture.image.prepared && texture.interval >= 8 &&
            texture.postponements < kPostponementMax && bytes_hash_frame_ > 0 &&
            bytes_hash_frame_ + bytes_hash > uint64_t(budget_hashes_) * 1024) {
          ++texture.postponements;
          texture.next = frame_ + 1 + (key & 1);
          valid_until = texture.next - 1;
          bo2zm::waits::g_hashes_postponed.fetch_add(1, std::memory_order_relaxed);
          prepare_outcome_ = 2;
          return;
        }
        texture.postponements = 0;
        // What this check reads, and in which stamp epoch (TextureUnwritten). Whatever it concludes, it
        // reads these bytes now: equal, read again, or copied for the hash thread.
        texture.stamp_base = uint32_t(start_raw);
        texture.stamp_base_bytes = uint32_t(extension);
        texture.stamp_mips = uint32_t(dir_mips);
        texture.stamp_mips_bytes = uint32_t(extension_mips);
        texture.check_epoch =
            bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed) ? bo2zm::page_writes::Epoch() : 0;
        bytes_hash_frame_ += bytes_hash;
        bo2zm::waits::g_bytes_hash.fetch_add(bytes_hash, std::memory_order_relaxed);
        const auto start_hash_raw = std::chrono::steady_clock::now();
        const uint8_t* const raw_base = memory_->TranslatePhysical(uint32_t(start_raw));
        const uint8_t* const raw_mips = extension_mips ? memory_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;
        // The sample goes before the full hash. If the full one matches, the sample is of that same content,
        // and it is the one stored.
        uint64_t sample = 0;
        if (with_sample) {
          sample = HashSample(raw_base, extension, 0);
          if (extension_mips) {
            sample = HashSample(raw_mips, extension_mips, sample);
          }
          bytes_sample_ += bytes_sample;
          ReportSampling(start_hash_raw);
        }
        const bool sample_equal = sample_useful && sample == texture.hash_sample;
        if (only_sample && sample_equal) {
          bo2zm::waits::g_ns_hash_raw.fetch_add(
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                            start_hash_raw)
                           .count()),
              std::memory_order_relaxed);
          ++texture.samples_consecutive;
          ++samples_hits_;
          bytes_saved_sample_ += bytes_full - bytes_sample;
          texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
          texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          prepare_outcome_ = 3;
          return;
        }
        if (only_sample) {
          // The sample changed: the full hash follows, although it was not in this frame's budget.
          ++samples_different_;
          bytes_hash_frame_ += bytes_full;
          bo2zm::waits::g_bytes_hash.fetch_add(bytes_full, std::memory_order_relaxed);
        }
        /*
         * New texture -> hash thread (bo2zm_native_texture_hash_thread; see PlanHash).
         * The plan is the same ReadLevel calls the usual path makes further down, with the same arguments, but
         * reading from a copy of the bytes this hash covers. If any read falls outside them, or ReadLevel
         * would go past memory, the texture continues on the usual path.
         */
        if (!texture.image.prepared && hashes_phase_ != kHashesOff && !diag_mips_ && tiled_fast_ >= 0) {
          std::array<size_t, 16> offsets{};
          std::array<size_t, 16> layer_level{};
          size_t bytes_plan = 0;
          for (uint32_t n = 0; n < texture.levels; ++n) {
            const uint32_t bx_n = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_n = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
            offsets[n] = bytes_plan;
            layer_level[n] = size_t(bx_n) * by_n * tf.bytes;
            bytes_plan += layer_level[n] * layers;
          }
          const uint32_t bx_base = (texture.image.width + tf.block - 1) / tf.block;
          const uint64_t mips_in_copy = (extension + 63) & ~uint64_t(63);  // where the copy of the mips starts
          uint32_t n_reads = 0;
          bool plan_ok = true;
          for (uint32_t c = 0; plan_ok && c < (read_base ? layers : 0u); ++c) {
            plan_ok = PlanReadHash(reads_plan_, n_reads, dir_base + c * stride_face, tiled_texture,
                                           pitch_blocks, uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf,
                                           base_ox, base_oy, blocks_x, blocks_y, bx_base, layer_level[0] * c,
                                           start_raw, extension, 0);
          }
          for (uint32_t n = 1; plan_ok && n < texture.levels; ++n) {
            const uint32_t s = level_packed == 0 ? 0 : std::min(n, level_packed);
            uint32_t ox = 0;
            uint32_t oy = 0;
            if (n >= level_packed) {
              OffsetPacked(width, height, tf.block, n, ox, oy);
            }
            const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
            for (uint32_t c = 0; plan_ok && c < layers; ++c) {
              plan_ok = PlanReadHash(reads_plan_, n_reads,
                                             dir_mips + regions[s].displacement + c * regions[s].stride,
                                             tiled_texture, regions[s].pitch_blocks, regions[s].row_bytes, tf,
                                             ox, oy, std::min(bx_guest, bx_host), std::min(by_guest, by_host),
                                             bx_host, offsets[n] + layer_level[n] * c, dir_mips, extension_mips,
                                             mips_in_copy);
            }
          }
          if (plan_ok && PlanHash(texture, key, n_reads, raw_base, extension, raw_mips, extension_mips,
                                       tf, (f[1] >> 6) & 0x3, bytes_plan, offsets, base, width, height,
                                       format, bytes_upload)) {
            prepare_outcome_ = 7;
            return;  // applying phase: the thread prepares it; the tail of this function is already done
          }
        }
        // A new texture's was already taken when it was looked up by content (ReuseByContent).
        uint64_t hash_raw = hash_known ? hash_raw_known : XXH3_64bits(raw_base, size_t(extension));
        if (extension_mips && !hash_known) {  // and the bytes of all the mips
          hash_raw = XXH3_64bits_withSeed(raw_mips, size_t(extension_mips), hash_raw);
        }
        bo2zm::waits::g_ns_hash_raw.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                          start_hash_raw)
                         .count()),
            std::memory_order_relaxed);
        if (prepare_new_) {
          CutSubstage(33, t_new);
        }
        const bool full_equal = texture.image.prepared && hash_raw == texture.hash_raw;
        if (sample_useful && !only_sample) {
          // The guard. This stable recheck computed both hashes.
          ++samples_checked_;
          ++samples_with_two_;
          if (sample_equal && !full_equal) {
            REXLOG_ERROR("[native] C3: the SAMPLE of texture {:08X} {}x{} format {} ({} KB of base and {} KB of mips) "
                         "did not see a change that the full hash did. Hash sampling is switched OFF (after {} rechecks "
                         "with both).",
                         uint32_t(start_raw), width, height, format, extension >> 10, extension_mips >> 10,
                         samples_checked_);
            sampling_hashes_ = 0;
          } else if (samples_checked_ == kSamplesACheck) {
            REXLOG_INFO("[native] C3: hash sampling: {} rechecks with both hashes, no disagreement. Sampling stays on, "
                        "with 1 in {} full.",
                        samples_checked_, sampling_hashes_);
          }
        }
        if (full_equal) {
          // The sample of this content, for the stable rechecks.
          if (with_sample && sampling_hashes_ > 0) {
            texture.hash_sample = sample;
            texture.sample_valid = true;
          }
          texture.samples_consecutive = 0;
          texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
          // At the maximum interval, from 32 to 39 depending on the texture, so they do not coincide.
          texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          prepare_outcome_ = 4;
          return;
        }
        if (texture.guard_stamps) {
          TextureChangedWithoutStamp(texture, width, height, format);  // the stamps missed this change
        }
        texture.sample_valid = false;  // the content changed, and with it the sample
        texture.samples_consecutive = 0;
        texture.hash_raw = hash_raw;
        AnnotateContentTexture(texture, key);  // measurement only: bo2zm_native_diag_reuse
      }
    }
    prepare_outcome_ = 6;  // read again (a new texture: read the first time)
    // Base level of each layer, untiled and in host byte order.
    const uint32_t blocks_x_host = (texture.image.width + tf.block - 1) / tf.block;
    const uint32_t blocks_y_host = (texture.image.height + tf.block - 1) / tf.block;
    const size_t bytes_layer = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
    if (volume && UpdateVolume(texture, f, tf, blocks_x, blocks_y, depth3d, blocks_x_host, blocks_y_host,
                                     pitch_blocks, valid_until, bytes_upload)) {
      return;  // updated (or found unchanged) by pages
    }
    if (volume) {
      texture.incremental.reset();  // the full read below replaces what the image holds: start over next time
    }
    std::vector<uint8_t>& data = temporal_;
    // All levels back to back, each one layer after layer (UploadTexture copies them one by one).
    std::array<size_t, 16> bytes_layer_level{};
    size_t bytes_data = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
      texture.offset_level[n] = uint32_t(bytes_data);
      bytes_layer_level[n] = size_t(bx_host) * by_host * tf.bytes;
      bytes_data += bytes_layer_level[n] * layers * (depth3d ? depth3d : 1);
    }
    const size_t previous_capacity = data.capacity();
    try {
      data.assign(bytes_data, 0);
    } catch (const std::bad_alloc&) {
      // Submitting here can re-enter texture preparation while the draw still holds references.
      // Keep the image and any pending bind intact, skip it for this frame, and retry next frame.
      RemoveContentTexture(texture, key);
      texture.hash_raw = previous_hash_raw;
      texture.sample_valid = false;
      texture.check_epoch = 0;
      texture.interval = 1;
      texture.next = frame_ + 1;
      texture.staging_failed_frame = frame_;
      slot = 0;
      valid_until = frame_;
      const uint64_t failures = ++staging_alloc_failures_;
      if ((failures & (failures - 1)) == 0) {
        try {
          const auto pool = pool_textures_.State();
          REXLOG_WARN("[native] texture staging allocation failed: requested {} bytes, previous capacity {} bytes, "
                      "{} failures; texture {:08X} {}x{} skipped for this frame; cache payload {} MB, "
                      "pool occupied {} MB of {} MB reserved",
                      bytes_data, previous_capacity, failures, base, width, height, bytes_textures_ >> 20,
                      pool.bytes_in_use >> 20, pool.bytes_reserved >> 20);
        } catch (const std::bad_alloc&) {
          // A diagnostic must not turn a handled allocation failure into a fatal one.
        }
      }
      return;
    }
    texture.staging_failed_frame = UINT64_MAX;
    const bool tiled = (f[0] >> 31) & 0x1;
    const auto start_volume = volume ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (volume && !ReadVolume(f, tf, blocks_x, blocks_y, depth3d, blocks_x_host,
                                blocks_y_host, pitch_blocks, tiled, data)) {
      Warn(33, "texture outside memory");
      if (!texture.image.prepared) {
        slot = 0;  // the image remains uninitialized
      }
      return;
    }
    if (volume) {  // which 3D textures are read again from guest memory, and why (ReportVolumes)
      VolumeReadInfo& v = volumes_read_[uint64_t(dir_base) << 32 | format << 26 | (depth3d & 0x3FF) << 16 |
                                          (height & 0xFF) << 8 | (width & 0xFF)];
      v.base = uint32_t(dir_base);
      v.format = format;
      v.width = width;
      v.height = height;
      v.depth3d = depth3d;
      ++v.reads;
      v.bytes += data.size();
      v.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                            start_volume)
                           .count());
      v.already_prepared += texture.image.prepared ? 1 : 0;
    }
    // Base level: not read if not needed (minimum level 1; it stays zeroed and the sampler never goes below
    // 1).
    for (uint32_t c = 0; c < (volume || !read_base ? 0u : layers); ++c) {
      const uint64_t address = dir_base + c * stride_face;
      if (!ReadLevel(address, tiled, pitch_blocks, uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf,
                     base_ox, base_oy, blocks_x, blocks_y, blocks_x_host, data.data() + bytes_layer * c)) {
        Warn(33, "texture outside memory");
        if (!texture.image.prepared) {
          slot = 0;  // the image remains uninitialized
        }
        return;
      }
    }
    // Mip levels. Those in the packed tail share a region and are located with OffsetPacked;
    // the rest, each in its own.
    for (uint32_t n = 1; n < texture.levels; ++n) {
      const uint32_t s = level_packed == 0 ? 0 : std::min(n, level_packed);
      uint32_t ox = 0;
      uint32_t oy = 0;
      if (n >= level_packed) {
        OffsetPacked(width, height, tf.block, n, ox, oy);
      }
      const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
      for (uint32_t c = 0; c < layers; ++c) {
        const uint64_t address = dir_mips + regions[s].displacement + c * regions[s].stride;
        if (!ReadLevel(address, tiled, regions[s].pitch_blocks, regions[s].row_bytes, tf, ox, oy,
                       std::min(bx_guest, bx_host), std::min(by_guest, by_host), bx_host,
                       data.data() + texture.offset_level[n] + bytes_layer_level[n] * c)) {
          Warn(39, "mip level outside memory: left at zero");
          break;
        }
      }
    }
    if (prepare_new_) {
      CutSubstage(34, t_new);
    }
    const auto order = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);
    // In one go (ChangeOrderBytes), with the same guard as the fast untiling.
    if (tiled_fast_ > 0 && order != xenos::Endian::kNone && (tf.unit_order == 2 || tf.unit_order == 4)) {
      // The first 200, then 1 in 64. Checking every texture, this guard never got to switch itself off (there
      // were only 1,024 textures) and cost 2.1 ms per MB on every new texture.
      ++orders_seen_;
      const bool check = orders_seen_ <= kLevelsACheck || orders_seen_ % kCheckOneOfEvery == 0;
      if (check) {
        check_tiled_ = data;
      }
      ChangeOrderBytes(data.data(), data.size(), tf.unit_order, uint32_t(order));
      if (check) {
        ++orders_checked_;
        UsualOrder(check_tiled_, tf.unit_order, order);
        if (check_tiled_ != data) {
          REXLOG_ERROR("[native] C3: the fast byte-order change does NOT match (unit {}, order {}, {} bytes). The fast "
                       "byte order and untiling are switched OFF.",
                       tf.unit_order, uint32_t(order), data.size());
          data.swap(check_tiled_);
          tiled_fast_ = 0;
        } else if (orders_checked_ == kLevelsACheck) {
          REXLOG_INFO("[native] C3: fast byte-order change: {} textures checked against the usual one, all equal.",
                      orders_checked_);
        }
      }
    } else {
      UsualOrder(data, tf.unit_order, order);
    }
    if (diag_mips_ && texture.levels > 1 && read_base && !volume) {
      ReviewMips(base, format, width, height, tf, texture, bytes_layer_level, data);
    }
    /*
     * For a new (unprepared) texture this hash decides nothing: it is uploaded regardless. 0 is stored, and
     * the first time its memory changes it will be computed and uploaded (at most, one extra upload).
     */
    const auto start_hash_data = std::chrono::steady_clock::now();
    const uint64_t hash = texture.image.prepared ? XXH3_64bits(data.data(), data.size()) : 0;
    bo2zm::waits::g_ns_hash_data.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                      start_hash_data)
                     .count()),
        std::memory_order_relaxed);
    if (texture.image.prepared && hash == texture.hash) {
      texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
      texture.next = frame_ + texture.interval;
      valid_until = texture.next - 1;
      return;
    }
    // Observing phase of the hash thread: what the usual path produces, to compare when collecting.
    if (comparison_texture_ == &texture) {
      AnnotateComparisonHash(texture, data);
    }
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.hash = hash;
    texture.data.swap(data);
    texture.needs_upload = true;
    textures_a_upload_.push_back(&texture);
    bo2zm::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    bo2zm::waits::g_bytes_uploaded.fetch_add(texture.data.size(), std::memory_order_relaxed);
    bytes_upload += (texture.data.size() + 3) & ~size_t(3);
    if (prepare_new_) {
      CutSubstage(35, t_new);
    }
  }

  // --- Measurement only: new textures with the content of another (bo2zm_native_diag_reuse) ---------------
  /*
   * What it is for. The game reloads the zone packs at new addresses every time it returns to a zone (in
   * the cache diagnostic, of 1,024 textures created, only 6 landed at an address already used). Since the
   * address is part of the key, a returning texture is a new texture: the image is created, bound,
   * untiled and uploaded again, even though the same image, with the same bytes, is still in the cache
   * under the old key and nothing uses it. This measures how often that happens:
   *   - how many new textures have the same content and shape as a live one (the image would be the same);
   *   - how many of those others are cold: not checked for more than kFramesNoUseForRelease frames,
   *     which is what eviction uses to mean "unused" (a texture in use is checked at least every 39
   *     frames). Those are the duplicates the cache holds that nothing in use depends on.
   * With MB, to see how much GPU memory and upload traffic is behind them. It changes nothing: no
   * decision of the ring reads what is recorded here.
   *
   * How. live_per_content_ holds every texture with a valid raw hash under its content key: the XXH3
   * of its shape without the address (the key words without the base and mip addresses, plus the
   * VkFormat, the host size, the levels and the layers) seeded with its raw hash (the guest bytes of base
   * and mips). Two textures with the same content key have the same bytes read with the same layout: the
   * same host data. It is maintained:
   *   - when the image is created (the previous one is removed if the image is recreated);
   *   - when the raw hash is set: the usual path of PrepareTexture, and CollectHashes (hash thread);
   *   - when the texture is released: ReleaseImages, which the gradual, batch and out-of-memory
   *     evictions all go through.
   * A new texture, once its first hash is known, looks at those with the same content key, at most
   * kReuseLookMax: O(1) per texture, never a walk over the live ones. Removing one is O(k), with k
   * the ones with the same content (the repeats; almost always 0 or 1). Candidates are validated when
   * looked at (still under that content key, with a prepared image and no pending hash thread job), so a
   * stale entry cannot count.
   */
  static constexpr uint32_t kReuseLookMax = 8;  // candidates checked per new texture
  static constexpr uint32_t kReuseDetails = 8;  // lines detailing the first matches

  bool DiagReuse() {
    if (diag_reuse_ < 0) {
      diag_reuse_ = REXCVAR_GET(bo2zm_native_diag_reuse) ? 1 : 0;
      REXLOG_INFO("[native] C3: measuring new textures with the content of another live one (bo2zm_native_diag_reuse) = "
                  "{}",
                  diag_reuse_ ? "YES (counting only: changes nothing)" : "no");
    }
    return diag_reuse_ > 0;
  }

  // The content key of a texture with a valid raw hash. Never 0 (0 = not recorded).
  static uint64_t KeyContentOf(uint64_t shape_content, VkFormat format, uint32_t levels, uint32_t width,
                               uint32_t height, uint32_t layers, uint64_t hash_raw) {
    const uint64_t shape_parts[4] = {shape_content, (uint64_t(format) << 32) | levels,
                                     (uint64_t(width) << 32) | height, layers};
    return XXH3_64bits_withSeed(shape_parts, sizeof(shape_parts), hash_raw) | 1;
  }
  static uint64_t KeyContent(const CachedTexture& texture) {
    return KeyContentOf(texture.shape_content, texture.image.format, texture.levels, texture.image.width,
                        texture.image.height, texture.layers, texture.hash_raw);
  }

  // The content index (live_per_content_) is kept while the measurement or the reuse uses it.
  bool ContentIndexed() { return DiagReuse() || TextureReuse(); }

  // Stops counting a texture (when it is released, when its image is recreated, or before recording it
  // with another hash).
  void RemoveContentTexture(CachedTexture& texture, uint64_t key) {
    if (!texture.key_content) {
      return;
    }
    const auto [since, until] = live_per_content_.equal_range(texture.key_content);
    for (auto it = since; it != until; ++it) {
      if (it->second == key) {
        live_per_content_.erase(it);
        break;
      }
    }
    if (live_per_content_.find(texture.key_content) == live_per_content_.end() && contents_different_) {
      --contents_different_;
    }
    texture.key_content = 0;
  }

  // With the raw hash just set. If the texture is new, counts whether another live one has the same
  // content and shape, and whether that other one is cold. Then records it under its content key. Ring
  // thread only.
  void AnnotateContentTexture(CachedTexture& texture, uint64_t key) {
    if (!ContentIndexed()) {
      return;
    }
    RemoveContentTexture(texture, key);
    const uint64_t content = KeyContent(texture);
    if (texture.content_per_measure) {
      texture.content_per_measure = false;
      ++reuse_new_;
      reuse_bytes_new_ += texture.bytes;
      const CachedTexture* equal = nullptr;
      const CachedTexture* cold = nullptr;
      uint32_t observed = 0;
      const auto [since, until] = live_per_content_.equal_range(content);
      for (auto it = since; it != until; ++it) {
        if (observed++ >= kReuseLookMax) {
          ++reuse_no_look_;
          break;
        }
        const auto other = textures_.find(it->second);
        if (other == textures_.end() || &other->second == &texture) {
          continue;
        }
        const CachedTexture& t = other->second;
        if (t.key_content != content || t.image.image == VK_NULL_HANDLE || !t.image.prepared ||
            t.hash_job) {
          continue;
        }
        equal = &t;
        if (t.frame != UINT64_MAX && t.frame + kFramesNoUseForRelease < frame_) {
          cold = &t;
          break;  // one cold one is enough
        }
      }
      if (equal) {
        ++reuse_match_;
        reuse_bytes_match_ += texture.bytes;
      }
      if (cold) {
        ++reuse_cold_;
        reuse_bytes_cold_ += texture.bytes;
      }
      if (equal && reuse_details_ < kReuseDetails) {
        ++reuse_details_;
        const CachedTexture& t = cold ? *cold : *equal;
        BO2ZM_REPORT_RING("[native] C3 reuse by content (measurement only): new texture {:08X} ({}x{}, "
                             "VkFormat {}, {} levels, {} layers, {} KB) has the same content and shape as {:08X}, {}",
                             texture.address, texture.image.width, texture.image.height,
                             uint32_t(texture.image.format), texture.levels, texture.layers, texture.bytes >> 10,
                             t.address,
                             cold ? fmt::format("not checked for {} frames", frame_ - t.frame)
                                  : std::string("which is still in use"));
      }
    }
    if (live_per_content_.find(content) == live_per_content_.end()) {
      ++contents_different_;
    }
    live_per_content_.emplace(content, key);
    texture.key_content = content;
  }

  // Every 10 s, if there were new textures with a hash. The cache snapshot (recorded, distinct contents)
  // is O(1).
  void ReportReuse() {
    if (diag_reuse_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_reuse_ < std::chrono::seconds(10)) {
      return;
    }
    report_reuse_ = now;
    if (reuse_new_) {
      const uint64_t pointed = uint64_t(live_per_content_.size());
      BO2ZM_REPORT_RING(
          "[native] C3 reuse by content (measurement only), last 10 s: {} new textures with a hash ({:.1f} "
          "MB); {} with the same content and shape as another live one ({:.1f} MB), {} of them with one not checked for "
          "more than {} frames ({:.1f} MB); {} with more than {} equal ones without a full look | in the cache: {} "
          "textures with a hash in {} different contents ({} repeated)",
          reuse_new_, double(reuse_bytes_new_) / 1048576.0, reuse_match_,
          double(reuse_bytes_match_) / 1048576.0, reuse_cold_, kFramesNoUseForRelease,
          double(reuse_bytes_cold_) / 1048576.0, reuse_no_look_, kReuseLookMax, pointed,
          contents_different_, pointed - std::min(pointed, contents_different_));
    }
    reuse_new_ = 0;
    reuse_match_ = 0;
    reuse_cold_ = 0;
    reuse_no_look_ = 0;
    reuse_bytes_new_ = 0;
    reuse_bytes_match_ = 0;
    reuse_bytes_cold_ = 0;
  }

  // Every 30 s, one line with what hash sampling has done (see PrepareTexture). The ring thread writes
  // it, but it is a single short line every 30 s.
  void ReportSampling(std::chrono::steady_clock::time_point now) {
    if (now - report_sampling_ < std::chrono::seconds(30)) {
      return;
    }
    if (report_sampling_ != std::chrono::steady_clock::time_point{}) {
      BO2ZM_REPORT_RING("[native] C3 hash sampling: {} rechecks settled by the sample ({:.1f} MB read and {:.1f} MB not "
                  "read), {} with a changed sample and {} with both hashes since the previous line; {} with both since "
                  "the start",
                  samples_hits_, double(bytes_sample_) / 1048576.0,
                  double(bytes_saved_sample_) / 1048576.0, samples_different_, samples_with_two_,
                  samples_checked_);
    }
    report_sampling_ = now;
    samples_hits_ = 0;
    samples_different_ = 0;
    samples_with_two_ = 0;
    bytes_sample_ = 0;
    bytes_saved_sample_ = 0;
  }

  // bo2zm_native_diag_mips: average color of each level of the first layer against the base's, for
  // DXT1/3/5 (average of the two colors of each block, in RGB565) and 8888. Only levels of 2x2 blocks or
  // more: the small ones are too little data for an average. Counts the textures with any level more than
  // 32/255 away from the base in any channel and logs the first 12 with their averages.
  void ReviewMips(uint32_t base, uint32_t format, uint32_t width, uint32_t height, const HostTextureFormat& tf,
                   const CachedTexture& texture, const std::array<size_t, 16>& bytes_layer_level,
                   const std::vector<uint8_t>& data) {
    const bool dxt = tf.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || tf.format == VK_FORMAT_BC2_UNORM_BLOCK ||
                     tf.format == VK_FORMAT_BC3_UNORM_BLOCK;
    if (!dxt && tf.format != VK_FORMAT_R8G8B8A8_UNORM) {
      return;
    }
    const auto average_of = [&](uint32_t n, std::array<double, 3>& rgb) {
      const size_t start = texture.offset_level[n];
      const size_t bytes = bytes_layer_level[n];
      if (start + bytes > data.size() || bytes < 4u * tf.bytes) {
        return false;
      }
      rgb = {0.0, 0.0, 0.0};
      size_t count = 0;
      for (size_t i = 0; i + tf.bytes <= bytes; i += tf.bytes) {
        const uint8_t* p = data.data() + start + i;
        if (dxt) {
          const uint8_t* color = tf.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? p : p + 8;
          for (uint32_t k = 0; k < 2; ++k) {
            const uint16_t v = uint16_t(color[2 * k] | (color[2 * k + 1] << 8));
            rgb[0] += ((v >> 11) & 31) * (255.0 / 31.0);
            rgb[1] += ((v >> 5) & 63) * (255.0 / 63.0);
            rgb[2] += (v & 31) * (255.0 / 31.0);
          }
          count += 2;
        } else {
          rgb[0] += p[0];
          rgb[1] += p[1];
          rgb[2] += p[2];
          count += 1;
        }
      }
      for (double& c : rgb) {
        c /= double(count);
      }
      return true;
    };
    std::array<double, 3> base_rgb{};
    if (!average_of(0, base_rgb)) {
      return;
    }
    ++mips_reviewed_;
    std::string levels = fmt::format("[0]={:.0f}/{:.0f}/{:.0f}", base_rgb[0], base_rgb[1], base_rgb[2]);
    double worst = 0.0;
    for (uint32_t n = 1; n < texture.levels; ++n) {
      std::array<double, 3> rgb{};
      if (!average_of(n, rgb)) {
        break;
      }
      for (uint32_t c = 0; c < 3; ++c) {
        worst = std::max(worst, std::abs(rgb[c] - base_rgb[c]));
      }
      levels += fmt::format(" [{}]={:.0f}/{:.0f}/{:.0f}", n, rgb[0], rgb[1], rgb[2]);
    }
    if (worst > 32.0) {
      if (++mips_rare_ <= 12) {
        REXLOG_WARN("[native] C3 diag mips: texture {:08X} {}x{} format {} with {} levels: one level is {:.0f} away "
                    "from the base; RGB averages {}",
                    base, width, height, format, texture.levels, worst, levels);
      }
    }
    if (mips_reviewed_ % 200 == 0) {
      REXLOG_INFO("[native] C3 diag mips: {} textures reviewed, {} with some level more than 32/255 away from the base",
                  mips_reviewed_, mips_rare_);
    }
  }

  // The blocks of one level (one layer) of a 2D texture or cubemap, from block (ox, oy) of its guest
  // region (the packed tail or the base of a small texture do not start at 0), to the host destination
  // bx_host blocks wide. Linear: row_bytes per row; tiled: pitch in blocks (GetTiledOffset2D). false if
  // it goes past memory.
  bool ReadLevel(uint64_t address, bool tiled, uint32_t pitch_blocks, uint64_t row_bytes,
                 const HostTextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                 uint8_t* target) {
    if (!bx || !by) {
      return true;
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t bound =
        tiled ? uint64_t(std::max<int64_t>(OffsetTiled2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u),
                                                                     pitch_blocks, log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + bound > kMemoryPhysical) {
      return false;
    }
    const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
    if (!tiled) {
      for (uint32_t y = 0; y < by; ++y) {
        std::memcpy(target + size_t(y) * bx_host * tf.bytes,
                    source + uint64_t(oy + y) * row_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
      }
      return true;
    }
    // The fast untiling (UntileLevel) when the block is 1, 2, 4, 8 or 16 bytes.
    if (tiled_fast_ < 0) {
      tiled_fast_ = REXCVAR_GET(bo2zm_native_fast_tiling) ? 1 : 0;
      REXLOG_INFO("[native] C3: fast untiling (bo2zm_native_fast_tiling) = {}",
                  tiled_fast_ ? "YES, checking the first levels against the usual one" : "no");
    }
    if (tiled_fast_ && tf.bytes == (1u << log2)) {
      switch (log2) {
        case 0: UntileLevel<0>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 1: UntileLevel<1>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 2: UntileLevel<2>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 3: UntileLevel<3>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        default: UntileLevel<4>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      }
      /*
       * Guard: the first kLevelsACheck levels are repeated on the usual path and compared byte by byte.
       * If one differs, the usual result is kept, a warning is logged and the fast path is off for the rest
       * of the run.
       */
      // The first 200, then 1 in 64 (checking every level, the guard cost 4.2 ms per MB).
      ++levels_seen_;
      if (levels_seen_ <= kLevelsACheck || levels_seen_ % kCheckOneOfEvery == 0) {
        ++levels_checked_;
        check_tiled_.assign(size_t(by) * bx_host * tf.bytes, 0);
        for (uint32_t y = 0; y < by; ++y) {
          std::memcpy(check_tiled_.data() + size_t(y) * bx_host * tf.bytes,
                      target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
        }
        ReadTiledLevelUsual(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
        bool equal = true;
        for (uint32_t y = 0; y < by && equal; ++y) {
          equal = std::memcmp(check_tiled_.data() + size_t(y) * bx_host * tf.bytes,
                                target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes) == 0;
        }
        if (!equal) {
          REXLOG_ERROR("[native] C3: the fast untiling does NOT match ({}x{} blocks of {} bytes, pitch {}, from {},{}). "
                       "It is switched OFF and the usual one is used.",
                       bx, by, tf.bytes, pitch_blocks, ox, oy);
          tiled_fast_ = 0;
        } else if (levels_checked_ == kLevelsACheck) {
          REXLOG_INFO("[native] C3: fast untiling: {} levels checked against the usual one, all equal. The fast one "
                      "stays.",
                      levels_checked_);
        }
      }
      return true;
    }
    ReadTiledLevelUsual(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
    return true;
  }

  // The original byte swap, word by word.
  static void UsualOrder(std::vector<uint8_t>& data, uint32_t unit, xenos::Endian order) {
    if (unit == 2 && order != xenos::Endian::kNone) {
      for (size_t i = 0; i + 1 < data.size(); i += 2) {
        uint16_t v;
        std::memcpy(&v, data.data() + i, 2);
        v = xenos::GpuSwap(v, order);
        std::memcpy(data.data() + i, &v, 2);
      }
    } else if (unit == 4 && order != xenos::Endian::kNone) {
      for (size_t i = 0; i + 3 < data.size(); i += 4) {
        uint32_t v;
        std::memcpy(&v, data.data() + i, 4);
        v = xenos::GpuSwap(v, order);
        std::memcpy(data.data() + i, &v, 4);
      }
    }
  }

  // The original untiling, block by block.
  static void ReadTiledLevelUsual(const uint8_t* source, uint32_t pitch_blocks, uint32_t log2, uint32_t bytes,
                                        uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                                        uint8_t* target) {
    for (uint32_t y = 0; y < by; ++y) {
      for (uint32_t x = 0; x < bx; ++x) {
        const int32_t displacement =
            OffsetTiled2D(int32_t(ox + x), int32_t(oy + y), pitch_blocks, log2);
        std::memcpy(target + (size_t(y) * bx_host + x) * bytes, source + displacement, bytes);
      }
    }
  }

  // Base level of a 3D texture, slice by slice (z, then y, then x), in the order vkCmdCopyBufferToImage
  // uploads it. Tiled: GetTiledOffset3D; linear: consecutive slices with rows and columns aligned to 32
  // blocks (GetGuestTextureLayout).
  // A tiled 3D texture by 4 KB pages. Its guest bytes as last read (raw) are compared with memory page by
  // page (by hash); only the 16-byte runs in pages that changed are untiled and byte-swapped into the host copy, and only
  // the rows they touch are uploaded (one region per slice). The first time (or after a full read) the state is built and
  // everything is uploaded once. Checked against an untiling texel by texel from the same bytes (the first 50 updates,
  // then 1 in 2,048): a difference switches it off for the rest of the run and the full path takes over. false: not handled
  // here (the full path runs).
  bool UpdateVolume(CachedTexture& texture, const uint32_t* f, const HostTextureFormat& tf, uint32_t blocks_x,
                         uint32_t blocks_y, uint32_t depth3d, uint32_t blocks_x_host, uint32_t blocks_y_host,
                         uint32_t pitch_blocks, uint64_t& valid_until, uint64_t& bytes_upload) {
    if (volume_incremental_ < 0) {
      volume_incremental_ = REXCVAR_GET(bo2zm_native_incremental_volume) ? 1 : 0;
    }
    const auto order = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);
    const bool tiled = (f[0] >> 31) & 0x1;
    if (volume_incremental_ == 0 || !tiled || tf.block != 1 || tf.bytes == 0 || tf.bytes > 16 ||
        (16 % tf.bytes) != 0 || texture.levels != 1 || texture.layers != 1 || !texture.image.prepared ||
        texture.in_flight || texture.hash_job || texture.needs_upload || blocks_x > blocks_x_host ||
        blocks_y > blocks_y_host ||
        (order != xenos::Endian::kNone &&
         !(tiled_fast_ > 0 && (tf.unit_order == 2 || tf.unit_order == 4)))) {
      return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const uint32_t address = uint32_t(uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF);  // as ReadVolume
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const int64_t bound_signed = OffsetTiled3D(int32_t((blocks_x + 31) & ~31u),
                                                            int32_t((blocks_y + 31) & ~31u),
                                                            int32_t((depth3d + 3) & ~3u), pitch_blocks, blocks_y, log2);
    if (bound_signed <= 0 || uint64_t(address) + uint64_t(bound_signed) > kMemoryPhysical) {
      return false;
    }
    const uint32_t bound = uint32_t(bound_signed);
    const uint8_t* live = memory_->TranslatePhysical(address);
    const uint32_t pages = (bound + 4095) / 4096;
    IncrementalVolume* v = texture.incremental.get();
    const bool new_value = !v || v->address != address || v->bound != bound || v->depth3d != depth3d ||
                       v->height != blocks_y_host || v->width_host != blocks_x_host;
    std::vector<uint32_t>& dirty = pages_dirty_;
    dirty.clear();
    if (new_value) {
      texture.incremental = std::make_unique<IncrementalVolume>();
      v = texture.incremental.get();
      v->address = address;
      v->bound = bound;
      v->depth3d = depth3d;
      v->height = blocks_y_host;
      v->width_host = blocks_x_host;
      v->row = blocks_x_host * tf.bytes;
      v->slice = v->row * blocks_y_host;
      v->unit_order = tf.unit_order;
      v->order = uint32_t(order);
      v->raw.assign(live, live + bound);
      v->host.assign(size_t(v->slice) * depth3d, 0);
      const uint32_t span = 16 / tf.bytes;
      std::vector<uint32_t> count(pages + 1, 0);
      for (uint32_t z = 0; z < depth3d; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          for (uint32_t x = 0; x < blocks_x; x += span) {
            const int32_t displacement = OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks,
                                                                   blocks_y, log2);
            const uint32_t bytes = std::min(span, blocks_x - x) * tf.bytes;
            if (displacement < 0 || uint64_t(displacement) + bytes > bound) {
              texture.incremental.reset();
              return false;
            }
            v->spans.push_back({uint32_t(displacement), z * v->slice + y * v->row + x * tf.bytes,
                                 uint16_t(bytes), uint16_t(x), uint16_t(y), uint16_t(z)});
            for (uint32_t p = uint32_t(displacement) / 4096; p <= (uint32_t(displacement) + bytes - 1) / 4096; ++p) {
              ++count[p];
            }
          }
        }
      }
      v->start_page.assign(pages + 1, 0);
      for (uint32_t p = 0; p < pages; ++p) {
        v->start_page[p + 1] = v->start_page[p] + count[p];
      }
      v->spans_page.assign(v->start_page[pages], 0);
      std::vector<uint32_t> full(v->start_page.begin(), v->start_page.end() - 1);
      for (uint32_t i = 0; i < v->spans.size(); ++i) {
        const IncrementalVolume::Span& t = v->spans[i];
        for (uint32_t p = t.source / 4096; p <= (t.source + t.bytes - 1) / 4096; ++p) {
          v->spans_page[full[p]++] = i;
        }
      }
      uint32_t used = 0;
      for (const IncrementalVolume::Span& t : v->spans) {
        used = std::max(used, (t.source + t.bytes + 4095) / 4096);
      }
      v->pages = used;
      v->hashes.assign(used, 0);
      for (uint32_t p = 0; p < used; ++p) {
        const uint32_t since = p * 4096;
        v->hashes[p] = XXH3_64bits(v->raw.data() + since, std::min<uint32_t>(4096, bound - since));
        dirty.push_back(p);
      }
      ++volume_states_;
      // PC diagnostics (bo2zm_debug_watch_volume): who writes the pages that hold texels.
      {
        std::vector<uint32_t> watched;
        for (uint32_t p = 0; p < v->pages; ++p) {
          if (v->start_page[p + 1] > v->start_page[p]) {
            watched.push_back(address + p * 4096);
          }
        }
        bo2zm::debug::WatchPhysicalPages(watched);
      }
    } else {
      // The pages that changed, copied into raw as they are now (the game may be writing: what is copied is what
      // is untiled, and a later write is seen next time). Either by each page's XXH3 against the one of
      // its bytes in raw, which reads the 2 MB once, or by comparing them with raw's copy, which reads 4 MB (on the
      // console ~1.4 ms per update that way, ~5 % of the ring thread); bo2zm_native_volume_by_hash. The stored
      // hash is taken from raw after the copy, so it always describes what was untiled, whichever way found it.
      const int32_t mode = REXCVAR_GET(bo2zm_native_volume_by_hash);
      const bool by_hash = mode == 2 ? (volume_detect_turn_++ & 1) != 0 : mode != 0;
      const auto before_detect = std::chrono::steady_clock::now();
      /*
       * By the writer's marks (bo2zm_volume_writes.cpp). The pages the game's writer (sub_828E5570) marked
       * since the last look are copied as they are now, without reading the rest. The check: the first 300 looks by
       * marks, then 1 in 64, also run the page loop below. A page it finds changed without a mark is a suspect: a job
       * worker may have stored it after the marks were taken here and marked it a moment later (seen on the PC). At
       * the next look a suspect whose mark has arrived is cleared; one still without a mark means another writer, and
       * the marks are off for the rest of the run.
       */
      const bool by_marks = bo2zm::volume_writes::UseMarks();
      const bool check_marks = by_marks && (++volume_mark_looks_ <= 300 || (volume_mark_looks_ & 63) == 0);
      if (by_marks) {
        marked_pages_.clear();
        bo2zm::volume_writes::TakeMarks(address, v->pages, marked_pages_);
        page_marked_.assign(v->pages, 0);
        for (const uint32_t p : marked_pages_) {
          if (p >= v->pages || v->start_page[p + 1] == v->start_page[p]) {
            continue;  // no texel of the volume in this page
          }
          page_marked_[p] = 1;
          const uint32_t since = p * 4096;
          const uint32_t bytes = std::min<uint32_t>(4096, bound - since);
          std::memcpy(v->raw.data() + since, live + since, bytes);
          v->hashes[p] = XXH3_64bits(v->raw.data() + since, bytes);
          dirty.push_back(p);
        }
        // The previous check's suspects: a late mark clears them.
        for (const uint32_t p : volume_mark_suspects_) {
          if (p < v->pages && page_marked_[p]) {
            ++volume_mark_late_;
          } else {
            ++volume_mark_misses_;
            bo2zm::volume_writes::Missed(address + p * 4096);
          }
        }
        volume_mark_suspects_.clear();
      } else {
        volume_mark_suspects_.clear();
      }
      if (!by_marks || check_marks) {
        for (uint32_t p = 0; p < v->pages; ++p) {
          if (v->start_page[p + 1] == v->start_page[p]) {
            continue;  // no texel of the volume in this page
          }
          const uint32_t since = p * 4096;
          const uint32_t bytes = std::min<uint32_t>(4096, bound - since);
          const bool changed = by_hash ? XXH3_64bits(live + since, bytes) != v->hashes[p]
                                       : std::memcmp(live + since, v->raw.data() + since, bytes) != 0;
          if (changed) {
            if (by_marks) {
              volume_mark_suspects_.push_back(p);  // judged at the next look
            }
            std::memcpy(v->raw.data() + since, live + since, bytes);
            v->hashes[p] = XXH3_64bits(v->raw.data() + since, bytes);
            dirty.push_back(p);
          }
        }
      }
      const int slot = by_marks ? 2 : by_hash ? 1 : 0;
      volume_detect_ns_[slot] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - before_detect)
                                              .count());
      ++volume_detects_[slot];
      volume_mark_checked_ += check_marks ? 1 : 0;
    }
    if (dirty.empty()) {
      ++volume_no_changes_;
      texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
      texture.next = frame_ + texture.interval;
      valid_until = texture.next - 1;
      volume_ns_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      return true;
    }
    // Untile the runs of those pages; the 32x32 tiles of each slice they touch.
    const uint32_t width_img = texture.image.width;
    const uint32_t height_img = texture.image.height;
    const uint32_t tiles_x = (width_img + 31) / 32;
    const uint32_t tiles_y = (height_img + 31) / 32;
    std::vector<uint8_t>& tiles = volume_tiles_;
    tiles.assign(size_t(tiles_x) * tiles_y * depth3d, 0);
    for (const uint32_t p : dirty) {
      for (uint32_t k = v->start_page[p]; k < v->start_page[p + 1]; ++k) {
        const IncrementalVolume::Span& t = v->spans[v->spans_page[k]];
        uint8_t* target = v->host.data() + t.target;
        std::memcpy(target, v->raw.data() + t.source, t.bytes);
        if (order != xenos::Endian::kNone) {
          ChangeOrderBytes(target, t.bytes, tf.unit_order, uint32_t(order));
        }
        const uint32_t x_end = t.x + t.bytes / tf.bytes - 1;  // a run never crosses a tile (16 bytes, aligned in x)
        tiles[(size_t(t.z) * tiles_y + (t.y >> 5)) * tiles_x + (t.x >> 5)] = 1;
        tiles[(size_t(t.z) * tiles_y + (t.y >> 5)) * tiles_x + (x_end >> 5)] = 1;
      }
    }
    // The guard: the same bytes untiled texel by texel (OffsetTiled3D, as ReadVolume's slow path) and
    // swapped in one go, compared with the whole host copy.
    ++volume_updates_;
    ++volume_updated_report_;
    // 1 in 2,048 after the first 50 (was 1 in 256): the texel-by-texel untiling of the 2 MB is a frame
    // hitch on the console, and in every run so far it has matched.
    if (volume_updates_ <= 50 || volume_updates_ % 2048 == 0) {
      std::vector<uint8_t>& expected = volume_check_;
      expected.assign(v->host.size(), 0);
      for (uint32_t z = 0; z < depth3d; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          for (uint32_t x = 0; x < blocks_x; ++x) {
            const int32_t d = OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks, blocks_y,
                                                      log2);
            std::memcpy(expected.data() + z * v->slice + y * v->row + x * tf.bytes, v->raw.data() + d, tf.bytes);
          }
        }
      }
      if (order != xenos::Endian::kNone) {
        ChangeOrderBytes(expected.data(), expected.size(), tf.unit_order, uint32_t(order));
      }
      ++volume_checked_;
      if (expected != v->host) {
        REXLOG_ERROR("[native] C3: the 3D texture by pages {:08X} does NOT match the whole read ({} pages changed). "
                     "Updating by pages is switched OFF (bo2zm_native_incremental_volume).",
                     address, dirty.size());
        volume_incremental_ = 0;
        texture.incremental.reset();
        return false;
      }
    }
    // Upload: each touched tile, its rows back to back in data (bufferRowLength 0: tightly packed). Consecutive
    // tiles of a tile row go as one region.
    texture.data.clear();
    texture.regions_partial.clear();
    for (uint32_t z = 0; z < depth3d; ++z) {
      for (uint32_t ly = 0; ly < tiles_y; ++ly) {
        for (uint32_t lx = 0; lx < tiles_x;) {
          if (!tiles[(size_t(z) * tiles_y + ly) * tiles_x + lx]) {
            ++lx;
            continue;
          }
          uint32_t end = lx + 1;
          while (end < tiles_x && tiles[(size_t(z) * tiles_y + ly) * tiles_x + end]) {
            ++end;
          }
          const uint32_t x0 = lx * 32, y0 = ly * 32;
          const uint32_t width = std::min(end * 32, width_img) - x0;
          const uint32_t rows = std::min(y0 + 32, height_img) - y0;
          VkBufferImageCopy region{};
          region.bufferOffset = texture.data.size();
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageOffset = {int32_t(x0), int32_t(y0), int32_t(z)};
          region.imageExtent = {width, rows, 1};
          for (uint32_t r = 0; r < rows; ++r) {
            const uint8_t* since = v->host.data() + z * v->slice + (y0 + r) * v->row + x0 * tf.bytes;
            texture.data.insert(texture.data.end(), since, since + size_t(width) * tf.bytes);
          }
          texture.regions_partial.push_back(region);
          lx = end;
        }
      }
    }
    texture.hash = 0;  // the full path's hash of the untiled data no longer describes the image
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.needs_upload = true;
    textures_a_upload_.push_back(&texture);
    bo2zm::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    bo2zm::waits::g_bytes_uploaded.fetch_add(texture.data.size(), std::memory_order_relaxed);
    bytes_upload += (texture.data.size() + 3) & ~size_t(3);
    volume_pages_ += dirty.size();
    volume_bytes_ += texture.data.size();
    volume_ns_ += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    return true;
  }
  int32_t volume_incremental_ = -1;  // bo2zm_native_incremental_volume, read on first use; 0 after a mismatch
  std::vector<uint32_t> pages_dirty_;
  std::vector<uint8_t> volume_tiles_;
  std::vector<uint8_t> volume_check_;
  uint64_t volume_states_ = 0, volume_no_changes_ = 0, volume_updates_ = 0, volume_checked_ = 0;
  uint64_t volume_updated_report_ = 0;
  uint64_t volume_pages_ = 0, volume_bytes_ = 0, volume_ns_ = 0;
  // Finding the changed pages, by method (0 = comparing with raw, 1 = by hash): time and count (the C3 line).
  uint64_t volume_detect_ns_[3] = {}, volume_detects_[3] = {};  // comparing, by hash, by the writer's marks
  uint32_t volume_detect_turn_ = 0;
  // By the writer's marks (bo2zm_volume_writes): looks so far, checks and misses in the report interval,
  // and the marked pages of a look.
  uint64_t volume_mark_looks_ = 0;
  uint64_t volume_mark_checked_ = 0;
  uint64_t volume_mark_misses_ = 0;
  uint64_t volume_mark_late_ = 0;  // suspects cleared by a mark that came after the check (report interval)
  std::vector<uint32_t> marked_pages_;
  std::vector<uint8_t> page_marked_;
  std::vector<uint32_t> volume_mark_suspects_;

  bool ReadVolume(const uint32_t* f, const HostTextureFormat& tf, uint32_t blocks_x,
                   uint32_t blocks_y, uint32_t depth3d, uint32_t blocks_x_host,
                   uint32_t blocks_y_host, uint32_t pitch_blocks, bool tiled,
                   std::vector<uint8_t>& data) {
    const uint64_t address = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                             : tf.bytes >= 2 ? 1 : 0;
    const uint64_t row = uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes;
    const uint64_t slice = row * ((blocks_y + 31) & ~uint32_t(31));
    const uint64_t bound =
        tiled ? uint64_t(std::max<int64_t>(
                      OffsetTiled3D(int32_t((blocks_x + 31) & ~31u),
                                              int32_t((blocks_y + 31) & ~31u),
                                              int32_t((depth3d + 3) & ~3u), pitch_blocks,
                                              blocks_y, log2),
                      0))
                : slice * depth3d;
    if (address + bound > kMemoryPhysical) {
      return false;
    }
    const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
    const size_t bytes_slice = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
    // The first volumes are also read texel by texel and compared (one log line with the result), both from one
    // snapshot of the guest bytes: the game rewrites BO2's model lighting volume while it is being read.
    static uint32_t volumes_checked = 0;
    const bool check = volumes_checked < 20;
    std::vector<uint8_t> snapshot;
    if (check) {
      snapshot.assign(source, source + std::min<uint64_t>(bound + 64, kMemoryPhysical - address));
      source = snapshot.data();
    }
    // In the Xenos 3D tiling, 16 bytes of consecutive x (4 texels of 4 bytes) are contiguous: x only
    // enters the address through micro & 15 (the low 4 bits of offset2) for x & (16 / bytes - 1), and nothing
    // carries out of those bits (OffsetTiled3D). One address per 16-byte run instead of one per texel:
    // BO2's 512x256x4 model lighting volume, re-read almost every frame, took ~20 ms per refresh on the Switch.
    if (tiled && tf.bytes >= 1 && tf.bytes <= 16 && (16 % tf.bytes) == 0 && !check) {
      const uint32_t span = 16 / tf.bytes;
      for (uint32_t z = 0; z < depth3d; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          uint8_t* row = data.data() + bytes_slice * z + size_t(y) * blocks_x_host * tf.bytes;
          for (uint32_t x = 0; x < blocks_x; x += span) {
            const uint64_t displacement = uint64_t(
                OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks, blocks_y, log2));
            const uint32_t n = std::min(span, blocks_x - x);
            std::memcpy(row + size_t(x) * tf.bytes, source + displacement, size_t(n) * tf.bytes);
          }
        }
      }
      return true;
    }
    for (uint32_t z = 0; z < depth3d; ++z) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x) {
          const uint64_t displacement =
              tiled ? uint64_t(OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z),
                                                         pitch_blocks, blocks_y, log2))
                      : z * slice + y * row + uint64_t(x) * tf.bytes;
          std::memcpy(data.data() + bytes_slice * z + (size_t(y) * blocks_x_host + x) * tf.bytes,
                      source + displacement, tf.bytes);
        }
      }
    }
    if (check && tiled && tf.bytes >= 1 && tf.bytes <= 16 && (16 % tf.bytes) == 0) {
      ++volumes_checked;
      const uint32_t span = 16 / tf.bytes;
      std::vector<uint8_t> fast(data.size(), 0);
      for (uint32_t z = 0; z < depth3d; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          uint8_t* row = fast.data() + bytes_slice * z + size_t(y) * blocks_x_host * tf.bytes;
          for (uint32_t x = 0; x < blocks_x; x += span) {
            const uint64_t displacement = uint64_t(
                OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks, blocks_y, log2));
            std::memcpy(row + size_t(x) * tf.bytes, source + displacement,
                        size_t(std::min(span, blocks_x - x)) * tf.bytes);
          }
        }
      }
      const size_t util = bytes_slice * depth3d;
      const bool equal = std::memcmp(fast.data(), data.data(), std::min(util, data.size())) == 0;
      if (!equal || volumes_checked == 20) {
        REXLOG_INFO("[native] C3 volume in 16-byte spans ({}x{}x{}, {} bytes per texel): {} ({} checked)",
                    blocks_x, blocks_y, depth3d, tf.bytes, equal ? "same as texel by texel" : "DIFFERENT",
                    volumes_checked);
      }
    }
    return true;
  }

  // A texture's VkImageCreateInfo, factored out so CreateTexture and CreateTextureInThread create exactly the
  // same image.
  static VkImageCreateInfo InfoImageTexture(VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                                             uint32_t depth3d, uint32_t levels) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = depth3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = format;
    info.extent = {width, height, depth3d ? depth3d : 1};
    info.mipLevels = std::max(levels, 1u);
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
  }

  // One allocation attempt. Recovery uses the same pool-first order as initial creation.
  bool TryAllocateTexture(NativeImage& image, const VkImageCreateInfo& info) {
    bool reserved = false;
    if (pool_textures_.IsActive()) {
      VkImage image_pool = VK_NULL_HANDLE;
      if (dfn_.vkCreateImage(device_, &info, nullptr, &image_pool) == VK_SUCCESS) {
        VkMemoryRequirements req{};
        dfn_.vkGetImageMemoryRequirements(device_, image_pool, &req);
        VkDeviceMemory block = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint32_t id_block = 0xFFFFFFFFu;
        if (pool_textures_.Reserve(req, block, offset, id_block) &&
            dfn_.vkBindImageMemory(device_, image_pool, block, offset) == VK_SUCCESS) {
          image.image = image_pool;
          image.guest_memory = VK_NULL_HANDLE;  // from the pool: not freed on its own
          image.pool_block = id_block;
          reserved = true;
        } else {
          if (id_block != 0xFFFFFFFFu) {
            pool_textures_.Free(id_block);
          }
          dfn_.vkDestroyImage(device_, image_pool, nullptr);
          pool_textures_.AnnotateDedicated();
        }
      }
    }
    if (!reserved) {
      image.pool_block = 0xFFFFFFFFu;
      reserved = rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
          image.guest_memory);
    }
    return reserved;
  }

  // `emergency` = false only from CollectBindings. Half the cache cannot be released there: that waits
  // for the GPU by submitting the work, and we are inside BeforeSubmit. Out of memory: false, and the
  // texture is recreated later.
  bool CreateTexture(NativeImage& image, VkFormat format, uint32_t width, uint32_t height,
                    uint32_t layers = 1, uint32_t depth3d = 0, uint32_t levels = 1, bool emergency = true) {
    const VkImageCreateInfo info = InfoImageTexture(format, width, height, layers, depth3d, levels);
    /*
     * The pool first, and the usual path only if it does not fit.
     *
     * A dedicated allocation per texture costs 1.9 ms of CPU on Horizon, measured inside the ioctls:
     * nvMapCreate 422 us, plus two GPU address reservations (127 us each) and two mappings (626 us each).
     * There are two of each because the dedicated allocation makes its own address and mapping, and then
     * nvk_image_plane_bind makes others for the plane. And the only thing the dedicated allocation buys is
     * compression, which our textures never use (nvk_image.c:843-846 returns false with
     * SAMPLED|TRANSFER_DST). So we paid double and got nothing.
     *
     * Sub-allocating from large slabs removes the allocation's address and mapping: 1.9 -> 0.75 ms. For a
     * burst of 15 textures in one frame, from 28.5 to 11.3 ms. That is what shows when entering a new zone
     * of the map.
     *
     * It does not change a single pixel: same tiling, same format, same pte_kind. Binding at an offset
     * keeps the block-linear layout (nvk_image.c:1700-1710) and is the normal path of any sub-allocator in
     * NVK.
     */
    bool reserved = TryAllocateTexture(image, info);
    if (reserved && test_no_memory_every_ > 0 &&
        ++texture_reserves_ % uint64_t(test_no_memory_every_) == 0) {
      // Test: undo the successful allocation and continue as if it had failed.
      DestroyImage(image);
      reserved = false;
      REXLOG_INFO("[native] C3: test, running out of memory is faked at allocation {}", texture_reserves_);
    }
    if (!reserved) {
      // Out of GPU memory. Instead of giving up (which ends in a black screen), half the cache is released
      // with the GPU idle and the allocation is retried once.
      // Freed pool slots already have backing memory. Prefer them before asking for more backing.
      // The caller may hold a CachedTexture reference: its image must survive the eviction.
      if (!emergency || !ReleaseTextureMemory(&image) || !TryAllocateTexture(image, info)) {
        REXLOG_ERROR("[native] C3: out of GPU memory for a {}x{} texture ({} levels, {} layers) and {}; {} textures and "
                     "{} MB remain",
                     width, height, std::max(levels, 1u), layers,
                     emergency ? "releasing the cache was not enough"
                                : "without releasing the cache (from CollectBindings; it is recreated at its next "
                                  "check)",
                     textures_.size(), bytes_textures_ >> 20);
        return false;
      }
    }
    image.width = width;
    image.height = height;
    image.format = format;
    image.prepared = false;
    return true;
  }

  bool UploadTexture(CachedTexture& texture) {
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload || !texture.needs_upload) {
      return upload != VK_NULL_HANDLE;
    }
    VkDeviceSize offset;
    if (!texture.regions_partial.empty()) {  // some rows of a 3D texture (UpdateVolume)
      Reserve(texture.data.size(), 16, offset);
      std::memcpy(upload_data_ + offset, texture.data.data(), texture.data.size());
      for (VkBufferImageCopy& region : texture.regions_partial) {
        region.bufferOffset += offset;
      }
      gpu_.CopyBufferToImage(upload, upload_, texture.image.image, VK_IMAGE_LAYOUT_GENERAL,
                             uint32_t(texture.regions_partial.size()), texture.regions_partial.data());
      texture.regions_partial.clear();
      texture.data.clear();
      texture.needs_upload = false;
      ++uploads_texture_;
      return true;
    }
    /*
     * If the hash thread prepares it, its data does not exist yet: its space is reserved (same size, same
     * point) and the thread writes it there; CollectHashes waits for it before submitting. The barrier
     * and the copy are recorded below as usual: the GPU does not read the upload buffer until
     * vkQueueSubmit.
     */
    if (texture.hash_job && texture.hash_job != kJobHashPublished) {
      const size_t bytes = BytesHashPlanned(texture);
      Reserve(bytes, 16, offset);  // BC: offset multiple of the block
      PublishHash(texture, upload_data_ + offset, bytes);
    } else {
      Reserve(texture.data.size(), 16, offset);  // BC: offset multiple of the block
      std::memcpy(upload_data_ + offset, texture.data.data(), texture.data.size());
    }
    /*
     * If its vkBindImageMemory is still on the bind thread, neither the barrier nor the copy can be
     * recorded for the image (it has no memory). The data is already in the upload buffer; the barrier and
     * the copy are recorded in CollectBindings, in this same upload buffer before it is closed
     * (BeforeSubmit).
     */
    if (texture.in_flight && !PostponeCopy(texture, offset, upload)) {
      CollectBindings(true);  // inconsistent index (PostponeCopy already logged it and switched off): collect everything here
    }
    if (!texture.in_flight && texture.image.image != VK_NULL_HANDLE) {
      if (!texture.image.prepared) {
        Barrier(upload, texture.image.image, texture.layers);
        texture.image.prepared = true;
      }
      RecordCopyTexture(upload, texture, offset);
    }
    texture.needs_upload = false;
    /*
     * The copy of the pixels is not kept. They are already in the upload buffer, and to know whether the
     * texture changes its hashes are kept, not the bytes. Keeping the copy would make the texture cache take
     * the same space again in CPU RAM (150-680 MB), enough for std::bad_alloc in the main menu with the cache
     * at 682 MB.
     */
    // The larger buffer is kept as temporal_ (so the next new texture does not ask the system for memory
    // again or touch fresh pages); the other is released.
    if (texture.data.capacity() > temporal_.capacity()) {
      temporal_.swap(texture.data);
    }
    std::vector<uint8_t>().swap(texture.data);
    ++uploads_texture_;
    return true;
  }

  // One range per mip level (with all its layers). Factored out for UploadTexture and for the deferred
  // copies of CollectBindings.
  void RecordCopyTexture(VkCommandBuffer upload, const CachedTexture& texture, VkDeviceSize offset) {
    std::array<VkBufferImageCopy, 16> copies{};
    for (uint32_t n = 0; n < texture.levels; ++n) {
      VkBufferImageCopy& copy = copies[n];
      copy.bufferOffset = offset + texture.offset_level[n];
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, n, 0, texture.layers};
      copy.imageExtent = {std::max(texture.image.width >> n, 1u), std::max(texture.image.height >> n, 1u),
                           texture.depth3d ? texture.depth3d : 1};
    }
    gpu_.CopyBufferToImage(upload, upload_, texture.image.image, VK_IMAGE_LAYOUT_GENERAL,
                           texture.levels, copies.data());
  }

  // A texture's view, the same for SlotView and for the deferred views of CollectBindings.
  VkResult CreateViewTexture(VkImage image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap,
                             VkImageView& view) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = heap == 2   ? VK_IMAGE_VIEW_TYPE_CUBE
                    : heap == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                  : VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.components = MappingComponents(swizzle, swizzle_host);
    info.subresourceRange = {IsDepth(format)
                                 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                 : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                             0, VK_REMAINING_MIP_LEVELS, 0, heap == 2 ? 6u : 1u};  // with its mips
    return dfn_.vkCreateImageView(device_, &info, nullptr, &view);
  }

  // The views_ key (image, heap and both swizzles), factored out for CollectBindings.
  static uint64_t KeyView(VkImage image, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap) {
    return XXH3_64bits_withSeed(&image, sizeof(image),
                                (uint64_t(heap) << 40) | (uint64_t(swizzle) << 16) | swizzle_host);
  }

  // Heap slot (0 for 2D textures, 2 for cubemaps) for a view of the image with that swizzle.
  uint32_t SlotView(VkImage image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host,
                       uint32_t heap = 0) {
    const uint64_t key = KeyView(image, swizzle, swizzle_host, heap);  // The same key
    if (const auto it = views_.find(key); it != views_.end()) {
      return it->second.slot;
    }
    // An image whose vkBindImageMemory is still on the bind thread. The view carries the image's GPU
    // address, which comes from the bind: the slot is reserved now, and the view is created and written in
    // CollectBindings, before submission (UPDATE_AFTER_BIND descriptors).
    if (!images_in_flight_.empty() && images_in_flight_.count(image)) {
      return SlotViewInFlight(key, image, format, swizzle, swizzle_host, heap);
    }
    View view;
    view.image = image;
    view.heap = heap;
    if (CreateViewTexture(image, format, swizzle, swizzle_host, heap, view.view) != VK_SUCCESS) {
      Warn(34, "could not create a texture's view");
      return 0;
    }
    view.slot = ReserveSlot(heap);
    if (!view.slot) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
      Warn(35, "texture heap full");
      ++slots_full_;
      return 0;
    }
    WriteImage(heap, view.slot, view.view);
    views_.emplace(key, view);
    views_per_image_[image].push_back(key);
    return view.slot;
  }

  // point: no filtering (depth textures).
  uint32_t SlotSampler(const uint32_t* f, bool point, uint32_t mip_min) {
    // Diagnostic: filters the game requests that this renderer does not apply (anisotropy, mip bias and
    // maximum mip level; the bias is not even part of the key). Each new value is logged once.
    {
      const uint32_t aniso = (f[3] >> 25) & 0x7;
      const int32_t bias = int32_t(f[4] << 10) >> 22;  // lod_bias: 10 signed bits, 5 fractional
      const uint32_t mip_max = (f[4] >> 6) & 0xF;
      const uint32_t index_bias = uint32_t(bias + 512);
      if (!((aniso_seen_ >> aniso) & 1u) || !biases_seen_[index_bias] || !((mip_max_seen_ >> mip_max) & 1u)) {
        aniso_seen_ |= 1u << aniso;
        biases_seen_[index_bias] = true;
        mip_max_seen_ |= 1u << mip_max;
        REXLOG_INFO("[native] C4 filters requested by the game (new value): anisotropic {}, mip bias {:.3f}, maximum "
                    "mip level {}, mag/min/mip filters {}/{}/{}{}",
                    aniso, float(bias) / 32.0f, mip_max, (f[3] >> 19) & 0x3, (f[3] >> 21) & 0x3,
                    (f[3] >> 23) & 0x3, point ? " (point sampling)" : "");
      }
    }
    /*
     * Trilinear -> bilinear, for measurement only (scene pass analysis).
     *
     * The game requests mag/min/mip filters 1/1/1, i.e. mipmapMode LINEAR. On Maxwell (GM20B) a trilinear
     * sample takes the texture unit two cycles (two bilinear ones) and a bilinear sample only one: with the
     * filter between levels at NEAREST the TMU's work is halved without touching a single ALU instruction.
     * It is the only lever that separates the two:
     *   - if the scene drops by ~half of the TMU's share, sampling dominates;
     *   - if it does not move, the ALU dominates and the TMU has headroom.
     * A mip level jump is visible on receding surfaces (the PS2 "shimmer"). For measurement only. Off by
     * default: at 0 this is two bool comparisons per new sampler (samplers are cached) and not one more
     * instruction on the draw path.
     *
     * The modified mip field is part of the sampler key, so toggling at runtime creates two different
     * samplers and nothing has to be invalidated: the alternation works.
     */
    uint32_t field_filters = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
    if (mip_point_test_ && ((field_filters >> 4) & 0x3) == 1) {
      field_filters &= ~(0x3u << 4);  // LINEAR -> NEAREST between mip levels
    }
    // With bo2zm_native_diag_min_mip the minimum level rises (without passing the maximum) and goes into
    // the key that way: each value has its own samplers, and going back to 0 reuses the usual ones.
    const uint32_t field_mips = bo2zm::native::SamplerMipRange((f[4] >> 2) & 0xFF, mip_min);
    // The fetch constant's LOD bias (word 4, bits 12-21, signed with 5 fraction bits) is part of the key:
    // BO2 sharpens some textures with -1, -2 and -7 (C4 "filters requested" lines).
    const int32_t bias_lod = int32_t(f[4] << 10) >> 22;
    const uint64_t key = uint64_t(((f[0] >> 10) & 0x1FF) | (field_filters << 9) | (field_mips << 21) |
                                    ((f[5] & 0x3) << 29) | (point ? 0x80000000u : 0u)) |
                           (uint64_t(uint32_t(bias_lod) & 0x3FF) << 32);
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
      return it->second.second;
    }
    static constexpr VkSamplerAddressMode kModes[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    const bool mirror = vulkan_device_->properties().samplerMirrorClampToEdge;
    const auto mode = [&](uint32_t value) {
      const VkSamplerAddressMode m = kModes[value & 0x7];
      return m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !mirror
                 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : m;
    };
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter =
        ((f[3] >> 19) & 0x3) == 1 && !point ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter =
        ((f[3] >> 21) & 0x3) == 1 && !point ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const uint32_t mip = (field_filters >> 4) & 0x3;  // already includes the point mip test
    info.mipmapMode =
        mip == 1 && !point ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = mode(f[0] >> 10);
    info.addressModeV = mode(f[0] >> 13);
    info.addressModeW = mode(f[0] >> 16);
    info.minLod = float(field_mips & 0xF);  // with the mip diagnostic if it is enabled
    // With real mips, the fetch constant's maximum level also limits (like the emulation's sampler);
    // single-level textures are not affected.
    info.maxLod = mip == 2 ? info.minLod + 0.25f : std::max(info.minLod, float((f[4] >> 6) & 0xF));
    info.borderColor = (f[5] & 0x3) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // The game's own filtering, as Xenia's samplers apply it. The LOD bias goes to mipLodBias (Vulkan
    // adds it to explicit LODs too, like D3D's MipLODBias). Anisotropy: the fetch constant's aniso_filter (word 3,
    // bits 25-27: 0 off, 1 = 1:1, 2 = 2:1, 3 = 4:1 ...) gives at most 2^(n-1) samples; BO2 asks 2:1 and 4:1 on most
    // world textures. Before, both were only logged, and textures came out blurrier than on Xenos (the hands up close).
    const auto& properties_game = vulkan_device_->properties();
    if (!point) {
      // +-15: maxSamplerLodBias of desktop NVIDIA and of NVK (the SDK's Properties do not carry the limit).
      info.mipLodBias = std::clamp(float(bias_lod) / 32.0f, -15.0f, 15.0f);
      const uint32_t aniso_game = (f[3] >> 25) & 0x7;
      if (aniso_game >= 2 && aniso_game <= 5 && properties_game.samplerAnisotropy &&
          info.minFilter == VK_FILTER_LINEAR && info.maxLod > info.minLod) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(float(1u << (aniso_game - 1)), properties_game.maxSamplerAnisotropy);
      }
    }
    // bo2zm_native_anisotropic. Only linear samplers with mips (maxLod above minLod); the value is read at
    // start-up, so it does not need to be in the key: every sampler is created with the same one.
    {
      static const uint32_t aniso_requested = uint32_t(std::max(0, int32_t(REXCVAR_GET(bo2zm_native_anisotropic))));
      const auto& properties = vulkan_device_->properties();
      if (aniso_requested > 1 && properties.samplerAnisotropy && info.minFilter == VK_FILTER_LINEAR &&
          info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR && info.maxLod > info.minLod) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::max(info.maxAnisotropy,
                                      std::max(1.0f, std::min(float(aniso_requested), properties.maxSamplerAnisotropy)));
        if (!aniso_annotated_) {
          aniso_annotated_ = true;
          REXLOG_INFO("[native] C4 anisotropic filtering {} on the linear samplers with mips (requested {}, device cap "
                      "{}); single-level and point ones unchanged",
                      info.maxAnisotropy, aniso_requested, properties.maxSamplerAnisotropy);
        }
      }
    }
    VkSampler sampler;
    if (dfn_.vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) {
      Warn(36, "could not create a sampler");
      return 0;
    }
    const uint32_t slot = ReserveSlot(3);
    if (!slot) {
      dfn_.vkDestroySampler(device_, sampler, nullptr);
      Warn(37, "sampler heap full");
      return 0;
    }
    WriteSampler(slot, sampler);
    samplers_.emplace(key, std::make_pair(sampler, slot));
    return slot;
  }

  // Category clipped to 1x1 (0 = none), with its alternating test.
  uint32_t TestScissor() {
    const int32_t category = REXCVAR_GET(bo2zm_native_test_scissor);
    if (category <= 0) {
      return 0;
    }
    const int32_t alternate = REXCVAR_GET(bo2zm_native_test_scissor_alternate_s);
    if (alternate <= 0) {
      return uint32_t(category);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool with_scissor = (seconds / alternate) % 2 == 1;
    if (with_scissor != alternation_scissor_annotated_) {
      alternation_scissor_annotated_ = with_scissor;
      REXLOG_INFO("[native] C2 test scissor: {}", with_scissor ? "clipping to 1x1" : "normal");
    }
    return with_scissor ? uint32_t(category) : 0;
  }

  /*
   * The ring's verdict for vegetation filtered in the game (bo2zm_d3d_game_vegetation). If the draw
   * carries the game's verdict (the kVeg* flags of its record), the ring's own is computed from this
   * draw's registers and PS: the criterion of Draw's early discard, minus the earlier exits that have
   * no side effects (primitive type, vertex input, diagnostics) and do not draw either. It goes to the
   * guard (AnnotateVegetationRing) and, if Draw reaches its early discard, it must have decided the
   * same (AnnotateVegetationModel otherwise).
   */
  void VerdictVegetation(const DrawRequest& p) {
    vegetation_computed_ = false;
    if (!(p.vegetation_game & kVegHas)) {
      return;
    }
    const uint32_t* r = p.registers;
    VegetationDetail a;
    if (r) {
      a.mode = r[gr::XE_GPU_REG_RB_MODECONTROL];
      a.mask = r[gr::XE_GPU_REG_RB_COLOR_MASK];
      a.control = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    }
    a.vs = p.vs ? int32_t(p.vs->number) : -1;
    a.ps = p.ps ? int32_t(p.ps->number) : -1;
    if (p.ps) {
      a.outputs = p.ps->outputs;
      a.discards = p.ps->discards;
    }
    if (r && p.vs && p.ps && (a.mode & 0x7) == uint32_t(xenos::EdramMode::kColorDepth)) {
      bool color = false;
      for (uint32_t i = 0; i < 4; ++i) {
        color = color || (((a.mask >> (i * 4)) & 0xF) && ((p.ps->outputs >> i) & 0x1));
      }
      const bool test_alpha = ((a.control >> 3) & 0x1) && (a.control & 0x7) != 7;
      a.structural = !color && (test_alpha || p.ps->discards || (p.ps->outputs & 0x10));
    }
    a.settings = (cvars_per_frame_ ? ps_only_alpha_frame_ : PsOnlyAlpha()) && no_vegetation_;
    a.occlusion = occlusion_open_;
    vegetation_ring_ = a.structural && a.settings && !a.occlusion;
    vegetation_computed_ = true;
    vegetation_flags_ = p.vegetation_game;
    vegetation_detail_ = a;
    AnnotateVegetationRing(p.vegetation_game, a);
  }

  // The setting, or its alternating test.
  bool PsOnlyAlpha() {
    const int32_t alternate = REXCVAR_GET(bo2zm_native_ps_alpha_only_alternate_s);
    if (alternate <= 0) {
      return REXCVAR_GET(bo2zm_native_ps_alpha_only);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool only_alpha = (seconds / alternate) % 2 == 1;
    if (only_alpha != alternation_only_alpha_annotated_) {
      alternation_only_alpha_annotated_ = only_alpha;
      REXLOG_INFO("[native] C2 color writes: {}",
                  only_alpha ? "removed in colorless passes" : "all");
    }
    return only_alpha;
  }

  // The setting, or its alternating test.
  bool NoPsNoColor() {
    const int32_t alternate = REXCVAR_GET(bo2zm_native_no_ps_without_color_alternate_s);
    if (alternate <= 0) {
      return REXCVAR_GET(bo2zm_native_no_ps_without_color);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool no_ps = (seconds / alternate) % 2 == 1;
    if (no_ps != alternation_ps_annotated_) {
      alternation_ps_annotated_ = no_ps;
      REXLOG_INFO("[native] C2 fragment stage: {}",
                  no_ps ? "removed in colorless draws" : "always built");
    }
    return no_ps;
  }

  // 1/size of a slot's host image, in the shared constants. With size 0 (a texture that could not be
  // prepared) 0 is written: the shader does not use it because it does not sample either.
  // The fetch constant's scale, which the library's shaders multiply every fetch by ({name}_FetchScale):
  // 2^exp_adjust (word 3, bits 13-18) and, with num_format integer (word 3, bit 0), the format's maximum, since an
  // integer read returns the raw value. BO2's tonemap samples the scene with exp_adjust +1 (it was half as bright),
  // and its luminance chain reads 16-bit integer textures with exp_adjust -11 (value x 2048 back to the value).
  static float MaxWhole(uint32_t format) {
    switch (xenos::TextureFormat(format)) {
      case xenos::TextureFormat::k_8:
      case xenos::TextureFormat::k_8_A:
      case xenos::TextureFormat::k_8_B:
      case xenos::TextureFormat::k_8_8:
      case xenos::TextureFormat::k_8_8_8_8:
      case xenos::TextureFormat::k_8_8_8_8_A:
      case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
        return 255.0f;
      case xenos::TextureFormat::k_2_10_10_10:
      case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
        return 1023.0f;
      case xenos::TextureFormat::k_16:
      case xenos::TextureFormat::k_16_16:
      case xenos::TextureFormat::k_16_16_16_16:
        return 65535.0f;
      default:
        return 1.0f;
    }
  }
  static void WriteScale(uint32_t* shared, uint32_t reg_index, const uint32_t* fetch) {
    float scale = std::ldexp(1.0f, int32_t(fetch[3] << 13) >> 26);
    if (fetch[3] & 0x1) {
      scale *= MaxWhole(fetch[1] & 0x3F);
    }
    std::memcpy(shared + WordScale(reg_index), &scale, sizeof(scale));
  }

  // Normalized 2D coordinates address the texture the fetch constant describes, but a resolved texture's
  // host image is as wide as the resolve's pitch: BO2's 880-wide scene is resolved to an 896-wide image and its
  // upscale to 1280 would sample 0..1 across all 896 columns, so the 16 empty ones would show as a black strip at the
  // right edge (seen in RenderDoc). The translated shaders multiply normalized 2D coordinates by guest size / host
  // size (written here, bytes 560 + slot * 8) when the host image is larger; 1 otherwise.
  static void WriteScaleCoords(uint32_t* shared, uint32_t reg_index, const uint32_t* fetch,
                                        uint32_t width, uint32_t height) {
    float scale[2] = {1.0f, 1.0f};
    if (((fetch[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked)) {
      const uint32_t width_guest = (fetch[2] & 0x1FFF) + 1;
      const uint32_t height_guest = ((fetch[2] >> 13) & 0x1FFF) + 1;
      if (width > width_guest) {
        scale[0] = float(width_guest) / float(width);
      }
      if (height > height_guest) {
        scale[1] = float(height_guest) / float(height);
      }
    }
    std::memcpy(shared + WordScaleCoords(reg_index), scale, sizeof(scale));
  }

  static void WriteInvSize(uint32_t* shared, uint32_t reg_index, uint32_t width, uint32_t height) {
    const float inv[2] = {width ? 1.0f / float(width) : 0.0f, height ? 1.0f / float(height) : 0.0f};
    std::memcpy(shared + WordInvSize(reg_index), inv, sizeof(inv));
  }

  bool BeginPass(const uint32_t* r, const uint64_t keys[5], uint32_t pitch,
                   uint64_t key_pass, const ShaderInput* ps) {
    start_pass_ = time_ ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    if (!PrepareEmpty()) {
      return false;
    }
    std::array<NativeImage*, 5> images{};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        images[i] = context_->TargetColor(uint32_t(keys[i] >> 24) & 0xFFF,
                                              uint32_t(keys[i] >> 16) & 0xF, pitch);
        if (!images[i]) {
          return Reject(40, "there is no color target");
        }
        if (i == 0) context_->BeginColorPass(*images[i], r, ps);
      }
    }
    if (keys[4]) {
      images[4] = context_->TargetDepth(uint32_t(keys[4] >> 24) & 0xFFF,
                                                  uint32_t(keys[4] >> 16) & 0x1, pitch);
      if (!images[4]) {
        return Reject(41, "there is no depth target");
      }
    }
    (void)r;
    // The render targets are already resolved above; that segment is closed.
    const auto after_targets = time_ ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    if (time_) {
      stages_ns_[9] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          after_targets - start_pass_).count());
    }
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    std::array<VkImageView, 5> views{};
    uint32_t formats[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
      if (images[i]) {
        width = std::min(width, images[i]->width);
        height = std::min(height, images[i]->height);
        views[i] = images[i]->view;
        formats[i] = uint32_t(images[i]->format);
      }
    }
    // Shadow map: a depth-only target with a pitch of 1600 or more (same as bo2zm_native_skip_shadows).
    const bool is_shadows = pitch >= 1600 && formats[4] && !formats[0] && !formats[1] &&
                            !formats[2] && !formats[3];
    // bo2zm_native_shadow_min. If C2 says this is the overlay pass of the shadow map, each draw is validated
    // for it (and the pass must be depth-only).
    pass_overlay_shadow_ = images[4] != nullptr && context_->OverlayShadowPass(images[4], is_shadows);
    // GPU time per pass type (C2 report), by render target width. Shadows are recognized by being
    // depth-only, not by measuring 1600 or more. With the scene at 1920 (a 1080p test build) the width no
    // longer tells them apart: the scene fell into the shadow bucket and vanished from its own category.
    category_pass_ = TargetCategory(pitch, keys);
    context_->MarkGpu(category_pass_);
    const bool shadows_no_load =
        is_shadows && REXCVAR_GET(bo2zm_native_shadow_pass_without_load);
    const VkRenderPass pass = PassOf(formats, shadows_no_load ? kLoadIgnore : kLoadRead);
    if (pass == VK_NULL_HANDLE) {
      return Reject(42, "could not create the render pass");
    }
    const VkFramebuffer framebuffer = FramebufferOf(pass, views, width, height);
    if (framebuffer == VK_NULL_HANDLE) {
      return Reject(43, "could not create the framebuffer");
    }
    if (time_) {
      stages_ns_[10] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - after_targets).count());
    }
    const auto before_open = time_ ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const VkCommandBuffer cmd = context_->CommandsJob();
    if (!cmd) {
      return false;
    }
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    // Only the rectangle the game uses (see bo2zm_native_useful_area_pass).
    uint32_t height_pass = height;
    if (area_util_) {
      const auto it = height_useful_.find(pitch);
      if (it != height_useful_.end() && it->second < height) {
        height_pass = it->second;
      }
    }
    start.renderArea.extent = {width, height_pass};
    // bo2zm_native_diag_clears. What this pass loads and stores of each render target, to compare with
    // what is cleared of it (C2 clears per target). Before opening the pass.
    for (const NativeImage* image : images) {
      if (image) {
        context_->AnnotatePassArea(image, width, height_pass);
      }
    }
    // The statistics cover the whole pass, from here to after EndRenderPass.
    stats_pass_ = context_->BeginStats(category_pass_);
    const auto before_start = std::chrono::steady_clock::now();
    PassBarrier(cmd);
    gpu_.BeginPass(cmd, start);
    ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_start).count());
    if (time_) {
      stages_ns_[11] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_open).count());
    }
    pass_active_ = true;
    ++passes_begun_;
    texels_passes_ += uint64_t(width) * height_pass;  // how much tile is loaded by loadOp = LOAD
    {
      // Same criterion as the per-category GPU time: the render target's pitch decides.
      const uint32_t category = TargetCategory(pitch, keys);
      texels_per_category_[category] += uint64_t(width) * height_pass;
      ++passes_per_category_[category];
    }
    pass_commands_ = cmd;
    draws_in_pass_ = 0;  // position within the pass (deferred sky report)
    pass_key_ = key_pass;
    std::memcpy(pass_keys_, keys, sizeof(pass_keys_));  // the bytes that produce pass_key_
    pass_keys_valid_ = true;
    ++pass_series_;  // the cached framing depends on the pass size and scale
    pass_generation_ = context_->GenerationCommands();
    pass_width_ = width;
    pass_height_ = height;
    // Only the shadow map is drawn at a size other than the one the guest thinks.
    pass_scale_ = (is_shadows && pitch && width && width != pitch)
                       ? float(width) / float(pitch)
                       : 1.0f;
    std::copy(std::begin(formats), std::end(formats), std::begin(pass_formats_));
    pass_rp_ = pass;
    // Phase 0b (bo2zm_native_pipeline_across_passes). Vulkan keeps the bound pipeline across passes of the
    // same buffer: the pass's first draw only binds again if its key differs. A new buffer still starts
    // with nothing bound (Draw, recording_generation_).
    if (!pipeline_between_passes_) {
      pipeline_bound_ = VK_NULL_HANDLE;
    } else if (pipeline_bound_ != VK_NULL_HANDLE) {
      ++passes_with_pipeline_;
    }
    state_recorded_ = false;
    if (pipeline_bound_ == VK_NULL_HANDLE) {
      eds_valid_ = false;  // pass that forgets the pipeline (without 0b): set everything again
    }
    return true;
  }

  // ZCULL: three load modes. kLoadClear is needed to clear the depth of images created without
  // TRANSFER_DST, the only ones eligible for a ZCULL plane.
  enum : uint32_t { kLoadRead = 0, kLoadIgnore = 1, kLoadClear = 2 };

  VkRenderPass PassOf(const uint32_t formats[5], uint32_t mode_load = kLoadRead) {
    // The mode is part of the key: two render passes with the same formats but a different loadOp are
    // different.
    const uint64_t key =
        XXH3_64bits_withSeed(formats, sizeof(uint32_t) * 5, mode_load);
    if (const auto it = passes_.find(key); it != passes_.end()) {
      return it->second;
    }
    const VkRenderPass pass = CreatePass(formats, mode_load);  // the usual code, factored out
    if (pass == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
    passes_.emplace(key, pass);
    return pass;
  }

  // The plain loop for 16-bit indices (byte-swapped or not), untouched.
  static void Indices16Scalar(const uint8_t* data, uint32_t count, uint16_t* output, bool flip, uint32_t& min,
                                 uint32_t& max) {
    min = 0xFFFF;
    max = 0;
    if (flip) {
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t raw;
        std::memcpy(&raw, data + size_t(i) * 2, 2);
        const uint16_t v = std::byteswap(raw);
        output[i] = v;
        min = std::min<uint32_t>(min, v);
        max = std::max<uint32_t>(max, v);
      }
    } else {
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t v;
        std::memcpy(&v, data + size_t(i) * 2, 2);
        output[i] = v;
        min = std::min<uint32_t>(min, v);
        max = std::max<uint32_t>(max, v);
      }
    }
  }

  // The same loop with NEON, 16 indices per iteration (vrev16 swaps the bytes of each 16-bit index, the
  // same as std::byteswap), and the remainder with the plain loop. NEON loads and stores need no
  // alignment on AArch64.
  static void Indices16Neon(const uint8_t* data, uint32_t count, uint16_t* output, bool flip, uint32_t& min,
                              uint32_t& max) {
#if defined(__aarch64__)
    uint32_t i = 0;
    uint32_t mn = 0xFFFF;
    uint32_t mx = 0;
    if (count >= 16) {
      uint16x8_t min_a = vdupq_n_u16(0xFFFF);
      uint16x8_t min_b = min_a;
      uint16x8_t max_a = vdupq_n_u16(0);
      uint16x8_t max_b = max_a;
      // Two loops (swapping is fixed per draw): no decision inside the iteration.
      if (flip) {
        for (; i + 16 <= count; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2)));
          const uint16x8_t b = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2 + 16)));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      } else {
        for (; i + 16 <= count; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2));
          const uint16x8_t b = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2 + 16));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      }
      mn = vminvq_u16(vminq_u16(min_a, min_b));
      mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
    }
    for (; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, data + size_t(i) * 2, 2);
      if (flip) {
        v = std::byteswap(v);
      }
      output[i] = v;
      mn = std::min<uint32_t>(mn, v);
      mx = std::max<uint32_t>(mx, v);
    }
    min = mn;
    max = mx;
#else
    Indices16Scalar(data, count, output, flip, min, max);
#endif
  }

  // The 16-bit indices of a draw (see bo2zm_native_indices_neon). Ring only.
  void Indices16(const uint8_t* data, uint32_t count, uint16_t* output, bool flip, uint32_t& min,
                   uint32_t& max) {
    if (indices_neon_ < 0) {
      indices_neon_ = REXCVAR_GET(bo2zm_native_indices_neon) ? 1 : 0;
      REXLOG_INFO("[native] C6 16-bit indices: {}",
                  indices_neon_ ? "with NEON; the first 20,000 draws are checked and then 1 in 4,096"
                                : "with the usual loop (bo2zm_native_indices_neon = false)");
    }
    if (indices_neon_ == 0) {
      Indices16Scalar(data, count, output, flip, min, max);
      return;
    }
    Indices16Neon(data, count, output, flip, min, max);
    const uint64_t n = ++indices_neon_draws_;
    if (n > 20000 && (n & 4095) != 0) {
      return;
    }
    indices_neon_test_.resize(count);
    uint32_t mn = 0;
    uint32_t mx = 0;
    Indices16Scalar(data, count, indices_neon_test_.data(), flip, mn, mx);
    ++indices_neon_checked_;
    if (mn != min || mx != max ||
        (count && std::memcmp(indices_neon_test_.data(), output, size_t(count) * 2) != 0)) {
      // The plain path stays for this draw and for the rest of the run.
      if (count) {
        std::memcpy(output, indices_neon_test_.data(), size_t(count) * 2);
      }
      min = mn;
      max = mx;
      indices_neon_ = 0;
      REXLOG_ERROR("[native] C6 16-bit indices: MISMATCH between NEON and the usual loop ({} indices, {}; min {} versus "
                   "{}, max {} versus {}). Off for the rest of the session: the usual loop",
                   count, flip ? "flipped" : "not flipped", min, mn, max, mx);
      return;
    }
    if (indices_neon_checked_ == 20000) {
      REXLOG_INFO("[native] C6 16-bit indices: 20,000 draws checked against the usual loop, 0 mismatches; "
                  "still checking 1 in 4,096");
    }
  }

  // The PassOf render pass without its cache, with nothing changed. The pipeline prewarm also uses it
  // from its thread (a compatible one: the same formats). It only reads its arguments.
  VkRenderPass CreatePass(const uint32_t formats[5], uint32_t mode_load) const {
    std::array<VkAttachmentDescription, 5> attachments{};
    std::array<VkAttachmentReference, 4> colors{};
    VkAttachmentReference depth{};
    uint32_t n = 0, n_colors = 0;
    for (uint32_t i = 0; i < 5; ++i) {
      if (!formats[i]) {
        continue;
      }
      VkAttachmentDescription& a = attachments[n];
      a.format = VkFormat(formats[i]);
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      // With the test active, the shadow map does not load its previous content: the game clears it before
      // drawing it, so fetching the whole map only to throw it away is wasted work.
      a.loadOp = mode_load == kLoadIgnore ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                 : mode_load == kLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = i != 4                       ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                        : mode_load == kLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.stencilStoreOp = i == 4 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (i < 4) {
        colors[n_colors++] = {n, VK_IMAGE_LAYOUT_GENERAL};
      } else {
        depth = {n, VK_IMAGE_LAYOUT_GENERAL};
      }
      ++n;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = n_colors;
    subpass.pColorAttachments = colors.data();
    subpass.pDepthStencilAttachment = formats[4] ? &depth : nullptr;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = n;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass;
    if (dfn_.vkCreateRenderPass(device_, &info, nullptr, &pass) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pass;
  }

  VkFramebuffer FramebufferOf(VkRenderPass pass, const std::array<VkImageView, 5>& views,
                              uint32_t width, uint32_t height) {
    struct Key {
      VkRenderPass pass;
      std::array<VkImageView, 5> views;
      uint32_t width, height;
    } key{pass, views, width, height};
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));
    if (const auto it = framebuffers_.find(hash); it != framebuffers_.end()) {
      return it->second;
    }
    std::array<VkImageView, 5> attachments{};
    uint32_t n = 0;
    for (VkImageView view : views) {
      if (view != VK_NULL_HANDLE) {
        attachments[n++] = view;
      }
    }
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = n;
    info.pAttachments = attachments.data();
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer;
    if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    framebuffers_.emplace(hash, framebuffer);
    framebuffers_views_[hash] = views;  // For ForgetView
    return framebuffer;
  }

  // The bright pass variants of bo2zm_sky_glow (1 natural, 2 soft), compiled with the same DXC
  // options as the library.
  VkShaderModule ModuleVariant(size_t index, const uint32_t* spirv, size_t bytes) {
    if (!modules_variants_created_[index]) {
      modules_variants_created_[index] = true;
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &modules_variants_[index]) != VK_SUCCESS) {
        modules_variants_[index] = VK_NULL_HANDLE;
      }
    }
    return modules_variants_[index];
  }

  // bo2zm_sky_glow (F4 menu, Graphics: 0 original, 1 natural, 2 soft), or its test rotation; logs
  // every change.
  int GlowSky() {
    int mode = bo2zm::settings::GlowSky();
    const int32_t alternate = REXCVAR_GET(bo2zm_native_smooth_glow_alternate_s);
    if (alternate > 0) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - start_shadows_)
                                .count();
      mode = int((seconds / alternate) % 3);
    }
    mode = std::clamp(mode, 0, 2);
    if (mode != glow_annotated_) {
      glow_annotated_ = mode;
      static constexpr const char* kNames[3] = {"original", "natural", "soft"};
      REXLOG_INFO("[native] sky glow: {} (frame {})", kNames[mode], frame_);
    }
    return mode;
  }

  /*
   * The early Z and invisible draws report, every 10 s like the others.
   *
   * What matters is not what is saved but the second figure: "CANNOT because they write depth"
   * is, draw by draw, the exact size of what only a depth pre-pass could address, and that is expensive
   * to implement. A small figure means a pre-pass is not worth the risk.
   */
  void ReportZEarly() {
    ++frames_z_;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report_z_).count() < 10) {
      return;
    }
    last_report_z_ = now;
    const uint64_t seen = frames_z_ - frames_z_previous_;
    const double frames = double(seen ? seen : 1);
    std::array<uint64_t, kZCounts> d{};
    uint64_t total = 0;
    for (uint32_t i = 0; i < kZCounts; ++i) {
      d[i] = counts_z_[i] - counts_z_previous_[i];
      counts_z_previous_[i] = counts_z_[i];
      total += d[i];
    }
    frames_z_previous_ = frames_z_;
    ReportSky(frames);  // goes before the !total cutoff, which has nothing to do with the sky
    if (!total) {
      return;
    }
    const uint64_t late = d[kZSet] + d[kZWritesZ] + d[kZStencil] + d[kZOcclusion];
    BO2ZM_REPORT_RING(
        "[native] C6 early Z per frame: {:.0f} draws take advantage of it, {:.0f} CANNOT because they write depth "
        "(those are the size of C88's prepass), {:.0f} because of stencil, {:.0f} inside an occlusion query; {:.0f} "
        "already tested before. Of those that forced shading first, {:.0f} % are fixed{}",
        double(d[kZSet]) / frames, double(d[kZWritesZ]) / frames,
        double(d[kZStencil]) / frames, double(d[kZOcclusion]) / frames,
        double(d[kZAlreadyEarly]) / frames,
        late ? 100.0 * double(d[kZSet]) / double(late) : 0.0,
        z_early_no_module_ ? fmt::format(" ({} shaders without a patched module)", z_early_no_module_)
                               : std::string());
    if (d[kInvisibleBlend] || d[kInvisibleAlpha] || d[kInvisibleOnlyColor]) {
      BO2ZM_REPORT_RING(
          "[native] C6 invisible draws per frame: {:.1f} with the blend that copies the target, {:.1f} with the alpha "
          "test at NEVER; {:.1f} keep only the depth",
          double(d[kInvisibleBlend]) / frames, double(d[kInvisibleAlpha]) / frames,
          double(d[kInvisibleOnlyColor]) / frames);
    }
  }

  /*
   * The deferred sky report. Without it there is no way to measure whether it works.
   *
   * What to check: "really postponed" must be just under 1 per frame in gameplay (the sky is a single draw
   * and is sometimes absent). "LOST" must be 0: if not, some path closes the pass without going through
   * FinishPass and the sky is being lost. And "average position" says how many draws of the pass were recorded
   * between the old place and the new one: exactly the amount of geometry that now covers the sky before
   * it is shaded. If it is small, so is the saving.
   */
  void ReportSky(double frames) {
    const uint64_t postponed = skies_postponed_ - sky_postponed_previous_;
    const uint64_t seen = sky_seen_ - sky_seen_previous_;
    if (!seen) {
      return;
    }
    std::array<uint64_t, kSkyReasons> per_reason{};
    uint64_t emitted = 0;
    for (uint32_t i = 0; i < kSkyReasons; ++i) {
      per_reason[i] = sky_emitted_[i] - sky_emitted_previous_[i];
      sky_emitted_previous_[i] = sky_emitted_[i];
      emitted += per_reason[i];
    }
    const uint64_t position_sum = sky_position_sum_ - sky_position_sum_previous_;
    const uint64_t candidates = sky_candidates_ - sky_candidates_previous_;
    sky_candidates_previous_ = sky_candidates_;
    sky_seen_previous_ = sky_seen_;
    sky_postponed_previous_ = skies_postponed_;
    sky_position_sum_previous_ = sky_position_sum_;
    /*
     * The line that decides whether the criterion works. "by the dome geometry" is the number that
     * matters: it must be 1.00 per frame. The "detected by hash" ones are by hash only and once gave several
     * per frame, which is what broke the image.
     */
    BO2ZM_REPORT_RING("[native] C6 sky: {:.2f} detected by hash and {:.2f} by the dome geometry (480 indices and first "
                "in the pass) per frame. The second has to be 1.00. GUARD: {} ({} frames with a dome looked at, {} with "
                "exactly one, maximum {} in one frame)",
                double(seen) / frames, double(candidates) / frames,
                sky_guard_ == kSkyPostponing  ? "PASSED, postponing"
                : sky_guard_ == kSkyDiscarded ? "REJECTED, nothing is postponed"
                                                     : "still observing",
                sky_guard_frames_, sky_guard_with_one_, sky_guard_max_);
    BO2ZM_REPORT_RING(
        "[native] C6 postponed sky ({}): {:.2f} detected per frame, {:.2f} really postponed ({} not postponable in "
        "total, {} with another sky already pending); emitted {} when a blended draw arrives, {} when an opaque draw "
        "that does not write Z arrives, {} when the pass closes, {} because of another sky; LOST {}; average position "
        "{:.0f} draws ahead of it (maximum {})",
        sky_postpone_on_ ? "on" : "OFF", double(seen) / frames,
        double(postponed) / frames, sky_no_postponable_, sky_two_in_pass_,
        per_reason[kSkyPerBlend], per_reason[kSkyPerNoZ], per_reason[kSkyAtPassEnd],
        per_reason[kSkyPerOtherSky], sky_lost_,
        emitted ? double(position_sum) / double(emitted) : 0.0, sky_position_max_);
  }

  /*
   * Closes the frame for the sky guard. See the block of fields.
   *
   * Runs once per frame and does nothing once the guard has decided, so its cost is one comparison. The
   * decision is taken only once per run and logged.
   */
  void CloseGuardSkyFrame() {
    const uint32_t in_this = sky_guard_in_frame_;
    sky_guard_in_frame_ = 0;
    if (sky_guard_ != kSkyObserving) {
      return;
    }
    /*
     * A frame without a sky says nothing about the criterion: menus, the logo and loading screens, which
     * come before any match. If they counted, the test would run out in the menu and the guard would switch
     * off for good without ever seeing the dome. Only frames in which the dome appears count.
     */
    if (in_this == 0) {
      return;
    }
    sky_guard_max_ = std::max(sky_guard_max_, in_this);
    if (in_this == 1) {
      ++sky_guard_with_one_;
    }
    if (++sky_guard_frames_ < kSkyFramesTest) {
      return;
    }
    /*
     * The test is over. It only switches on if it came out clean: never more than one, and enough frames
     * with exactly one. In any other case it stays off and the image is identical to the non-deferred one,
     * which is known to work.
     */
    if (sky_guard_max_ == 1 && sky_guard_with_one_ >= kSkyFramesWithOne) {
      sky_guard_ = kSkyPostponing;
      REXLOG_INFO("[native] C6 sky: guard PASSED ({} of {} frames with exactly one dome, never two). The sky is "
                  "postponed from now on, without changing a pixel",
                  sky_guard_with_one_, sky_guard_frames_);
    } else {
      sky_guard_ = kSkyDiscarded;
      REXLOG_WARN("[native] C6 sky: guard NOT passed (maximum {} domes in one frame, {} of {} frames with exactly one; "
                  "{} were needed). Nothing is postponed: the image stays exactly as it was",
                  sky_guard_max_, sky_guard_with_one_, sky_guard_frames_,
                  kSkyFramesWithOne);
    }
  }

  VkShaderModule ModuleOf(const ShaderInput& input) {
    if (const auto it = modules_.find(&input); it != modules_.end()) {
      return it->second;
    }
    const auto& spirv = bo2zm::shaderlib::Spirv(*input.shader);
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
      shader_module = VK_NULL_HANDLE;
    }
    modules_.emplace(&input, shader_module);
    return shader_module;
  }

  // Pixel shader module without the color writes, one per shader. If it cannot be pruned, the normal one
  // is returned: the image is the same, only the saving is lost.
  VkShaderModule ModuleOnlyAlpha(const ShaderInput& input) {
    if (const auto it = modules_only_alpha_.find(&input); it != modules_only_alpha_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleOf(input);
    }
    uint32_t removed = 0;
    const std::vector<uint32_t> pruned = PruneColorWrites(bo2zm::shaderlib::Spirv(*input.shader), removed);
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (!pruned.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = pruned.size() * sizeof(uint32_t);
      info.pCode = pruned.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C5a: PS n{} without color writes: {} removed, {} words of {} ({})",
                input.number, removed, pruned.size(), input.shader->spirv_words,
                shader_module != VK_NULL_HANDLE ? "module created" : "the normal one is used");
    modules_only_alpha_.emplace(&input, shader_module);
    return shader_module != VK_NULL_HANDLE ? shader_module : ModuleOf(input);
  }

  /*
   * Pixel shader module with EarlyFragmentTests declared, one per shader. If that is not possible (it
   * already had it, it writes gl_FragDepth...), the normal one is returned: the image is the same and only
   * the saving is lost. It is logged once per shader so the log shows which ones qualified.
   */
  VkShaderModule ModuleZEarly(const ShaderInput& input) {
    if (const auto it = modules_z_early_.find(&input); it != modules_z_early_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleOf(input);
    }
    const char* reason = "";
    const std::vector<uint32_t> patched = WithTestsEarly(bo2zm::shaderlib::Spirv(*input.shader), reason);
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (!patched.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = patched.size() * sizeof(uint32_t);
      info.pCode = patched.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C6 early Z: PS n{} {} ({} kills, {} words)", input.number,
                shader_module != VK_NULL_HANDLE
                    ? std::string("tests depth before shading")
                    : fmt::format("stays as it was: {}", reason),
                input.kills, input.shader->spirv_words);
    modules_z_early_.emplace(&input, shader_module);
    if (shader_module == VK_NULL_HANDLE) {
      ++z_early_no_module_;
    }
    return shader_module != VK_NULL_HANDLE ? shader_module : ModuleOf(input);
  }

  /*
   * The fixed state of a pipeline, exactly as PipelineOf sets it. It is PipelineOf's own code, kept here so
   * it can be shared: its eight declarations are references into PipelineFixedState. Used
   * by PipelineOf and by the guards of the canonical key (phase 0a) and of dynamic state (phases 1 and 2),
   * which thus compare against what the pipeline really carries and not against another copy of the same
   * computations. blend.pAttachments points to fixed.blends: the structure is not copied.
   */
  struct PipelineFixedState {
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineViewportStateCreateInfo view{};
    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineMultisampleStateCreateInfo sampling{};
    VkPipelineDepthStencilStateCreateInfo depth{};
    std::array<VkPipelineColorBlendAttachmentState, 4> blends{};
    uint32_t n_colors = 0;
    VkPipelineColorBlendStateCreateInfo blend{};
    PipelineFixedState() = default;
    PipelineFixedState(const PipelineFixedState&) = delete;
    PipelineFixedState& operator=(const PipelineFixedState&) = delete;
  };

  // warn = false from the prewarm thread (Warn belongs to the ring only).
  void FillStateFixed(const PipelineKey& key, PipelineFixedState& fixed, bool warn = true) {
    VkPipelineInputAssemblyStateCreateInfo& assembly = fixed.assembly;
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VkPrimitiveTopology(key.topology);
    assembly.primitiveRestartEnable = (key.rasterization & 0x8) ? VK_TRUE : VK_FALSE;
#if defined(__APPLE__)
    // Metal always restarts strips and fans at the all-ones index and cannot turn that off, so MoltenVK warns and
    // restarts anyway (rexglue-sdk issue #533). Asking for it states what the GPU does; the result is the same.
    if (key.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
        key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {
      assembly.primitiveRestartEnable = VK_TRUE;
    }
#endif

    VkPipelineViewportStateCreateInfo& view = fixed.view;
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo& rasterization = fixed.rasterization;
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = ((key.rasterization & 0x1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                           ((key.rasterization & 0x2) ? VK_CULL_MODE_BACK_BIT : 0);
    // PA_SU_SC_MODE_CNTL.face: 1 = the front face is clockwise (not confirmed on screen).
    rasterization.frontFace =
        (key.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    rasterization.depthBiasEnable = (key.rasterization & 0x10) ? VK_TRUE : VK_FALSE;
    rasterization.depthClampEnable = REXCVAR_GET(bo2zm_native_test_depth_clamp) ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const uint32_t d = key.depth;
    VkPipelineDepthStencilStateCreateInfo& depth = fixed.depth;
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (key.formats[4]) {
      depth.depthTestEnable = (d >> 1) & 0x1;
      depth.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
      depth.depthCompareOp = VkCompareOp((d >> 4) & 0x7);
      depth.stencilTestEnable = d & 0x1;
      depth.front.failOp = VkStencilOp((d >> 11) & 0x7);
      depth.front.passOp = VkStencilOp((d >> 14) & 0x7);
      depth.front.depthFailOp = VkStencilOp((d >> 17) & 0x7);
      depth.front.compareOp = VkCompareOp((d >> 8) & 0x7);
      if ((d >> 7) & 0x1) {
        depth.back.compareOp = VkCompareOp((d >> 20) & 0x7);
        depth.back.failOp = VkStencilOp((d >> 23) & 0x7);
        depth.back.passOp = VkStencilOp((d >> 26) & 0x7);
        depth.back.depthFailOp = VkStencilOp((d >> 29) & 0x7);
      } else {
        depth.back = depth.front;
      }
    }

    static constexpr VkBlendFactor kFactors[32] = {
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    const auto operation = [&](uint32_t op) {
      switch (op) {
        case 1:
          return VK_BLEND_OP_SUBTRACT;
        case 2:
          return VK_BLEND_OP_MIN;
        case 3:
          return VK_BLEND_OP_MAX;
        case 4:
          if (warn) {
            Warn(52, "blend with reverse subtract: subtract is used");
          }
          return VK_BLEND_OP_SUBTRACT;
        default:
          return VK_BLEND_OP_ADD;
      }
    };
    std::array<VkPipelineColorBlendAttachmentState, 4>& blends = fixed.blends;
    uint32_t& n_colors = fixed.n_colors;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!key.formats[i]) {
        continue;
      }
      const uint32_t m = key.blend[i];
      VkPipelineColorBlendAttachmentState& s = blends[n_colors++];
      s.colorWriteMask = (key.masks >> (i * 4)) & 0xF;
      const uint32_t source_kind = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
      const uint32_t source_kind_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7,
                     target_a = (m >> 24) & 0x1F;
      /*
       * Blending is only enabled if it is needed on the channels that are written.
       *
       * Requiring all six fields to be "1 x source + 0 x destination, ADD" would be too strict: if the mask
       * does not write RGB, the three color factors do not matter, and the same goes for alpha. With
       * blendEnable false the ROP does not have to read the destination or go through the blend unit. The
       * result is the same.
       */
      s.blendEnable = ColorBlendEnabled(s.colorWriteMask, m);
      s.srcColorBlendFactor = kFactors[source_kind];
      s.dstColorBlendFactor = kFactors[target];
      s.colorBlendOp = operation(op);
      s.srcAlphaBlendFactor = kFactors[source_kind_a];
      s.dstAlphaBlendFactor = kFactors[target_a];
      s.alphaBlendOp = operation(op_a);
    }
    VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = n_colors;
    blend.pAttachments = blends.data();
  }

  // One stencil face, in what Vulkan looks at in the pipeline (masks and reference are dynamic).
  static bool SameFaceStencil(const VkStencilOpState& a, const VkStencilOpState& b) {
    return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
  }

  /*
   * Two fixed states, field by field, in what Vulkan looks at: the blend equation only with blendEnable,
   * each half only if the mask writes its channels, the Z function only with a Z test, stencil operations
   * only with stencil. Returns the first differing field, or nullptr if there is none.
   */
  static const char* MismatchEffective(const PipelineFixedState& a, const PipelineFixedState& b) {
    if (a.assembly.topology != b.assembly.topology) {
      return "topology";
    }
    if (a.assembly.primitiveRestartEnable != b.assembly.primitiveRestartEnable) {
      return "primitiveRestartEnable";
    }
    if (a.view.viewportCount != b.view.viewportCount || a.view.scissorCount != b.view.scissorCount) {
      return "viewportCount or scissorCount";
    }
    const VkPipelineRasterizationStateCreateInfo& ra = a.rasterization;
    const VkPipelineRasterizationStateCreateInfo& rb = b.rasterization;
    if (ra.polygonMode != rb.polygonMode || ra.lineWidth != rb.lineWidth ||
        ra.rasterizerDiscardEnable != rb.rasterizerDiscardEnable || ra.depthClampEnable != rb.depthClampEnable) {
      return "polygonMode, lineWidth, rasterizerDiscardEnable or depthClampEnable";
    }
    if (ra.cullMode != rb.cullMode) {
      return "cullMode";
    }
    if (ra.frontFace != rb.frontFace) {
      return "frontFace";
    }
    if (ra.depthBiasEnable != rb.depthBiasEnable) {
      return "depthBiasEnable";
    }
    if (a.sampling.rasterizationSamples != b.sampling.rasterizationSamples) {
      return "rasterizationSamples";
    }
    const VkPipelineDepthStencilStateCreateInfo& pa = a.depth;
    const VkPipelineDepthStencilStateCreateInfo& pb = b.depth;
    if (pa.depthTestEnable != pb.depthTestEnable) {
      return "depthTestEnable";
    }
    if (pa.depthWriteEnable != pb.depthWriteEnable) {
      return "depthWriteEnable";
    }
    if (pa.depthTestEnable && pa.depthCompareOp != pb.depthCompareOp) {
      return "depthCompareOp";
    }
    if (pa.depthBoundsTestEnable != pb.depthBoundsTestEnable) {
      return "depthBoundsTestEnable";
    }
    if (pa.stencilTestEnable != pb.stencilTestEnable) {
      return "stencilTestEnable";
    }
    if (pa.stencilTestEnable && !SameFaceStencil(pa.front, pb.front)) {
      return "front (stencil)";
    }
    if (pa.stencilTestEnable && !SameFaceStencil(pa.back, pb.back)) {
      return "back (stencil)";
    }
    if (a.n_colors != b.n_colors || a.blend.attachmentCount != b.blend.attachmentCount ||
        a.blend.logicOpEnable != b.blend.logicOpEnable) {
      return "attachmentCount or logicOpEnable";
    }
    for (uint32_t i = 0; i < a.n_colors && i < 4; ++i) {
      const VkPipelineColorBlendAttachmentState& x = a.blends[i];
      const VkPipelineColorBlendAttachmentState& y = b.blends[i];
      if (x.colorWriteMask != y.colorWriteMask) {
        return "colorWriteMask";
      }
      if (x.blendEnable != y.blendEnable) {
        return "blendEnable";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x7) &&
          (x.srcColorBlendFactor != y.srcColorBlendFactor || x.dstColorBlendFactor != y.dstColorBlendFactor ||
           x.colorBlendOp != y.colorBlendOp)) {
        return "color equation";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x8) &&
          (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor || x.dstAlphaBlendFactor != y.dstAlphaBlendFactor ||
           x.alphaBlendOp != y.alphaBlendOp)) {
        return "alpha equation";
      }
    }
    return nullptr;
  }

  /*
   * Phase 0a (bo2zm_native_canonical_key). A draw's key in canonical form. Same criterion as
   * StateCanonical (the counter) with two differences: disabled blending becomes 1 x source + 0 x
   * destination, ADD (0x00010001; with 0, FillStateFixed would enable it with zero factors), and with
   * blending enabled the half of the equation (color or alpha) whose channels the mask does not write also
   * becomes that.
   */
  static void Canonicalize(PipelineKey& c) {
    constexpr uint32_t kDirect = 0x00010001u;  // 1 x source + 0 x destination, ADD, for color and alpha
    uint32_t masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!c.formats[i]) {
        c.blend[i] = 0;  // FillStateFixed skips targets not in the pass (Draw leaves them at 0)
        continue;
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      const bool writes_rgb = (mask & 0x7) != 0;
      const bool writes_alpha = (mask & 0x8) != 0;
      if (!ColorBlendEnabled(mask, m)) {
        c.blend[i] = kDirect;  // without blendEnable Vulkan ignores the equation
        continue;
      }
      uint32_t canonical = m;
      if (!writes_rgb) {
        canonical = (canonical & 0xFFFF0000u) | 0x00000001u;  // the color equation reaches no channel
      }
      if (!writes_alpha) {
        canonical = (canonical & 0x0000FFFFu) | 0x00010000u;  // neither does the alpha one
      }
      c.blend[i] = canonical;
    }
    c.masks = masks;
    uint32_t d = c.formats[4] ? c.depth : 0;  // ignored without a depth target
    d &= ~0x8u;                                      // bit 3 is not read
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // without stencil, its functions and operations do not count
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    c.depth = d;
  }

  /*
   * The phase 0a guard. Canonicalize only touches blending, masks and depth: the rest of the key must come
   * out the same, and the fixed state of both keys must match in everything Vulkan looks at
   * (MismatchEffective on FillStateFixed). On a difference the phase switches off for the rest of the run
   * and false is returned.
   */
  bool CheckKeyCanonical(const PipelineKey& raw_hash, const PipelineKey& canonical, uint64_t n) {
    ++canonical_checked_;
    const char* field = nullptr;
    if (raw_hash.vs != canonical.vs || raw_hash.ps != canonical.ps || raw_hash.input != canonical.input ||
        raw_hash.topology != canonical.topology || raw_hash.specialization != canonical.specialization ||
        raw_hash.rasterization != canonical.rasterization || raw_hash.padding != canonical.padding ||
        raw_hash.padding2 != canonical.padding2 ||
        std::memcmp(raw_hash.formats, canonical.formats, sizeof(raw_hash.formats)) != 0) {
      field = "shaders, input, topology, specialization, rasterization or formats";
    } else {
      PipelineFixedState a;
      PipelineFixedState b;
      FillStateFixed(raw_hash, a);
      FillStateFixed(canonical, b);
      field = MismatchEffective(a, b);
    }
    if (field) {
      key_canonical_off_ = true;
      key_canonical_ = false;
      canonical_valid_ = false;
      REXLOG_ERROR("[native] C6 canonical key: MISMATCH in {} (check {}: VS {} PS {}, blend {:08X} -> {:08X}, masks "
                   "{:04X} -> {:04X}, depth {:08X} -> {:08X}). Off for the rest of the session: lookups use the usual "
                   "key",
                   field, n, raw_hash.vs, raw_hash.ps, raw_hash.blend[0], canonical.blend[0], raw_hash.masks,
                   canonical.masks, raw_hash.depth, canonical.depth);
      return false;
    }
    if (n == kCanonicalACheck) {
      BO2ZM_REPORT_RING("[native] C6 canonical key: {} changed keys checked field by field against the usual one, 0 "
                           "mismatches; still checking 1 in 4096",
                           n);
    }
    return true;
  }

  /*
   * The key used to look up a draw's pipeline. The raw one (key) is used for everything else: the
   * deferred sky reads its blending (opaque_in_all) and the counter compares it. Phase 0a: canonical
   * form, remembering the previous draw's (the same raw key gives the same canonical one).
   */
  PipelineKey LookupKey(const PipelineKey& key) {
    PipelineKey c = key;
    if (key_canonical_) {
      if (canonical_valid_ && std::memcmp(&key, &canonical_raw_, sizeof(key)) == 0) {
        c = canonical_result_;
      } else {
        Canonicalize(c);
        if (std::memcmp(&c, &key, sizeof(c)) != 0) {
          const uint64_t n = ++canonical_changed_;
          if ((n <= kCanonicalACheck || (n & 4095) == 0) && !CheckKeyCanonical(key, c, n)) {
            c = key;  // the guard saw a difference: this draw, and the rest of the run, use the usual one
          }
        }
        canonical_raw_ = key;
        canonical_result_ = c;
        canonical_valid_ = key_canonical_;
      }
    }
    // Phases 1 and 2. What goes through vkCmdSet* leaves the key, and the flag (padding2) keeps these
    // pipelines apart from the usual ones in the map, in the direct-mapped cache and in PipelineOf's
    // shortcut.
    const uint32_t mode_eds = eds_mode_;
    if (mode_eds & kEds12) {
      c.depth = 0;
      c.rasterization = 0;
      c.topology = RepresentativeTopology(c.topology);
    }
    if (mode_eds & kEds3) {  // phase 2: blending and masks go through vkCmdSet*; targets stay in formats
      std::fill(std::begin(c.blend), std::end(c.blend), 0u);
      c.masks = 0;
    }
    c.padding2 = mode_eds;
    return c;
  }

  // Every 20 s, phase 0a (bo2zm_native_canonical_key).
  void ReportKeyCanonical() {
    const auto now = std::chrono::steady_clock::now();
    if (now - canonical_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = canonical_report_ == std::chrono::steady_clock::time_point{};
    canonical_report_ = now;
    const uint64_t changed = canonical_changed_ - canonical_changed_previous_;
    canonical_changed_previous_ = canonical_changed_;
    const uint64_t pipelines = pipelines_.size();
    const uint64_t new_ones = pipelines - std::min<uint64_t>(pipelines, canonical_pipelines_previous_);
    canonical_pipelines_previous_ = pipelines;
    if (first || (!changed && !new_ones)) {
      return;
    }
    BO2ZM_REPORT_RING("[native] C6 canonical key: {}; {} keys changed in 20 s ({} checked since the start, "
                         "{}); {} pipelines in the map, {} new in 20 s",
                         key_canonical_off_ ? "OFF by the guard"
                         : key_canonical_       ? "on"
                                                 : "off",
                         changed, canonical_checked_,
                         canonical_checked_ >= kCanonicalACheck ? "guard passed" : "checking", pipelines,
                         new_ones);
  }

  // Every 20 s, phase 0b (bo2zm_native_pipeline_across_passes).
  void ReportPipelineBetweenPasses() {
    const auto now = std::chrono::steady_clock::now();
    if (now - between_passes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = between_passes_report_ == std::chrono::steady_clock::time_point{};
    between_passes_report_ = now;
    const uint64_t passes = passes_begun_ - between_passes_passes_previous_;
    const uint64_t kept = passes_with_pipeline_ - between_passes_kept_previous_;
    between_passes_passes_previous_ = passes_begun_;
    between_passes_kept_previous_ = passes_with_pipeline_;
    if (first || !passes) {
      return;
    }
    BO2ZM_REPORT_RING("[native] C6 pipeline across passes: {}; {} passes begun in 20 s, {} with a bound "
                         "pipeline that is kept (its first draw does not bind again if the key is the same)",
                         pipeline_between_passes_ ? "kept" : "forgotten, as before", passes, kept);
  }

  /*
   * Dynamic state phases 1 and 2. The state that leaves the pipeline, with the Vulkan values the usual
   * pipeline would carry (the criterion of FillStateFixed, which is PipelineOf's).
   */
  struct EdsState {
    uint32_t face = 0;         // VkCullModeFlags
    uint32_t frontface = 0;       // VkFrontFace
    uint32_t topology = 0;    // VkPrimitiveTopology
    uint32_t restart = 0;     // primitiveRestartEnable
    uint32_t bias = 0;        // depthBiasEnable
    uint32_t test_z = 0;     // depthTestEnable
    uint32_t writes_z = 0;    // depthWriteEnable
    uint32_t function_z = 0;    // VkCompareOp
    uint32_t has_stencil = 0;     // stencilTestEnable
    uint32_t ahead[4] = {};  // failOp, passOp, depthFailOp (VkStencilOp) and compareOp (VkCompareOp)
    uint32_t behind[4] = {};   // the same for the back face
    uint32_t n_colors = 0;                    // phase 2: the pass's color targets, compacted as in PipelineOf
    uint32_t blend_active[4] = {};            // blendEnable
    gpu::BlendEquation equation[4] = {};  // color and alpha factors and operations
    gpu::ColorMask mask[4] = {};     // colorWriteMask
  };

  // Without EDS3 the dynamic topology must be of the same class as the pipeline's: the lookup key carries
  // one per class. Classes the ring does not draw stay as they are.
  static uint32_t RepresentativeTopology(uint32_t topology) {
    switch (ClassTopology(topology)) {
      case 1:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case 2:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      default:
        return topology;
    }
  }

  static const char* NameModeEds(uint32_t mode) {
    switch (mode & (kEds12 | kEds3)) {
      case kEds12:
        return "EDS1/EDS2 (phase 1)";
      case kEds3:
        return "EDS3 (phase 2)";
      case kEds12 | kEds3:
        return "EDS1/EDS2 and EDS3 (phases 1 and 2)";
      default:
        return "off: all the state in the pipeline, as before";
    }
  }

  // The dynamic state of a raw key in the given mode (kEds12 | kEds3).
  void StateEdsOf(const PipelineKey& c, EdsState& e, uint32_t mode) {
    e = EdsState{};
    if (mode & kEds12) {
      e.face = ((c.rasterization & 0x1) ? uint32_t(VK_CULL_MODE_FRONT_BIT) : 0u) |
               ((c.rasterization & 0x2) ? uint32_t(VK_CULL_MODE_BACK_BIT) : 0u);
      e.frontface = (c.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      e.restart = (c.rasterization & 0x8) ? 1u : 0u;
      e.bias = (c.rasterization & 0x10) ? 1u : 0u;
      e.topology = c.topology;
      if (c.formats[4]) {  // without a depth target everything stays zero, as in FillStateFixed
        const uint32_t d = c.depth;
        e.test_z = (d >> 1) & 0x1;
        e.writes_z = ((d >> 1) & 0x1) & ((d >> 2) & 0x1);
        e.function_z = (d >> 4) & 0x7;
        e.has_stencil = d & 0x1;
        e.ahead[0] = (d >> 11) & 0x7;
        e.ahead[1] = (d >> 14) & 0x7;
        e.ahead[2] = (d >> 17) & 0x7;
        e.ahead[3] = (d >> 8) & 0x7;
        if ((d >> 7) & 0x1) {
          e.behind[0] = (d >> 23) & 0x7;
          e.behind[1] = (d >> 26) & 0x7;
          e.behind[2] = (d >> 29) & 0x7;
          e.behind[3] = (d >> 20) & 0x7;
        } else {
          std::memcpy(e.behind, e.ahead, sizeof(e.behind));
        }
      }
    }
    if (mode & kEds3) {  // phase 2: like FillStateFixed, target by target and compacted
      for (uint32_t i = 0; i < 4; ++i) {
        if (!c.formats[i]) {
          continue;
        }
        const uint32_t a = e.n_colors++;
        const uint32_t m = c.blend[i];
        const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
        const uint32_t source_kind = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
        const uint32_t source_kind_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, target_a = (m >> 24) & 0x1F;
        e.mask[a] = mask;
        e.blend_active[a] = ColorBlendEnabled(mask, m) ? VK_TRUE : VK_FALSE;
        gpu::BlendEquation& q = e.equation[a];
        q.src_color = kFactorsBlend[source_kind];
        q.dst_color = kFactorsBlend[target];
        q.color_op = OperationBlend(op);
        q.src_alpha = kFactorsBlend[source_kind_a];
        q.dst_alpha = kFactorsBlend[target_a];
        q.alpha_op = OperationBlend(op_a);
      }
    }
  }

  /*
   * Records the vkCmdSet* calls for what changed since the last state set in the buffer (eds_recorded_),
   * or for all of it with everything = true, and tracks it. What Vulkan ignores in this draw (the Z function
   * without a Z test, stencil operations without stencil, the blend equation without blending) is not
   * re-recorded if only that changed; with everything = true it is recorded anyway, so everything is set at least
   * once in the buffer.
   */
  void EmitStateDynamic(VkCommandBuffer cmd, const EdsState& d, bool everything, uint32_t mode) {
    EdsState& g = eds_recorded_;
    eds_calls_[kEdsEverything] += everything ? 1 : 0;
    if (mode & kEds12) {
      if (everything || d.face != g.face) {
        gpu_.SetCullMode(cmd, d.face);
        g.face = d.face;
        ++eds_calls_[kEdsFace];
      }
      if (everything || d.frontface != g.frontface) {
        gpu_.SetFrontFace(cmd, d.frontface);
        g.frontface = d.frontface;
        ++eds_calls_[kEdsFrontface];
      }
      if (everything || d.topology != g.topology) {
        gpu_.SetTopology(cmd, d.topology);
        g.topology = d.topology;
        ++eds_calls_[kEdsTopology];
      }
      if (everything || d.restart != g.restart) {
        gpu_.SetPrimitiveRestart(cmd, d.restart != 0);
        g.restart = d.restart;
        ++eds_calls_[kEdsRestart];
      }
      if (everything || d.bias != g.bias) {
        gpu_.SetDepthBiasEnable(cmd, d.bias != 0);
        g.bias = d.bias;
        ++eds_calls_[kEdsBias];
      }
      if (everything || d.test_z != g.test_z) {
        gpu_.SetDepthTest(cmd, d.test_z != 0);
        g.test_z = d.test_z;
        ++eds_calls_[kEdsTestZ];
      }
      if (everything || d.writes_z != g.writes_z) {
        gpu_.SetDepthWrite(cmd, d.writes_z != 0);
        g.writes_z = d.writes_z;
        ++eds_calls_[kEdsWritesZ];
      }
      if (everything || (d.test_z && d.function_z != g.function_z)) {
        gpu_.SetDepthCompare(cmd, d.function_z);
        g.function_z = d.function_z;
        ++eds_calls_[kEdsFunctionZ];
      }
      if (everything || d.has_stencil != g.has_stencil) {
        gpu_.SetStencilTest(cmd, d.has_stencil != 0);
        g.has_stencil = d.has_stencil;
        ++eds_calls_[kEdsStencil];
      }
      const bool ahead = everything || (d.has_stencil && std::memcmp(d.ahead, g.ahead, sizeof(d.ahead)) != 0);
      const bool behind = everything || (d.has_stencil && std::memcmp(d.behind, g.behind, sizeof(d.behind)) != 0);
      if (ahead && behind && std::memcmp(d.ahead, d.behind, sizeof(d.ahead)) == 0) {
        gpu_.SetStencilOps(cmd, gpu::kStencilBoth, d.ahead);
        ++eds_calls_[kEdsStencilOps];
      } else {
        if (ahead) {
          gpu_.SetStencilOps(cmd, gpu::kStencilFront, d.ahead);
          ++eds_calls_[kEdsStencilOps];
        }
        if (behind) {
          gpu_.SetStencilOps(cmd, gpu::kStencilBack, d.behind);
          ++eds_calls_[kEdsStencilOps];
        }
      }
      if (ahead) {
        std::memcpy(g.ahead, d.ahead, sizeof(g.ahead));
      }
      if (behind) {
        std::memcpy(g.behind, d.behind, sizeof(g.behind));
      }
    }
    if (mode & kEds3) {  // phase 2
      const uint32_t n = d.n_colors;
      if (!n) {
        if (everything) {
          g.n_colors = 0;  // nothing set in this buffer: the first draw with color will set it all
        }
      } else {
        const bool other_n = everything || n != g.n_colors;
        bool active = other_n;
        bool equation = other_n;
        bool mask = other_n;
        for (uint32_t a = 0; a < n && a < 4; ++a) {
          active = active || d.blend_active[a] != g.blend_active[a];
          mask = mask || d.mask[a] != g.mask[a];
          if (!equation && d.blend_active[a]) {  // without blending the equation does not count; each half, with its channels
            const gpu::BlendEquation& x = d.equation[a];
            const gpu::BlendEquation& y = g.equation[a];
            equation = ((d.mask[a] & 0x7) != 0 &&
                        (x.src_color != y.src_color ||
                         x.dst_color != y.dst_color || x.color_op != y.color_op)) ||
                       ((d.mask[a] & 0x8) != 0 &&
                        (x.src_alpha != y.src_alpha ||
                         x.dst_alpha != y.dst_alpha || x.alpha_op != y.alpha_op));
          }
        }
        if (active) {
          gpu_.SetBlendEnable(cmd, n, d.blend_active);
          std::memcpy(g.blend_active, d.blend_active, sizeof(g.blend_active));
          ++eds_calls_[kEdsBlendActive];
        }
        if (equation) {
          gpu_.SetBlendEquations(cmd, n, d.equation);
          std::memcpy(g.equation, d.equation, sizeof(g.equation));
          ++eds_calls_[kEdsEquation];
        }
        if (mask) {
          gpu_.SetColorMasks(cmd, n, d.mask);
          std::memcpy(g.mask, d.mask, sizeof(g.mask));
          ++eds_calls_[kEdsMask];
        }
        g.n_colors = n;
      }
    }
  }

  /*
   * The guard of phases 1 and 2. What the ring believes is set in the buffer (eds_recorded_) must be what
   * this draw's usual pipeline would carry. That state comes from FillStateFixed with the raw key, the
   * same code PipelineOf uses to create the usual pipelines (not another copy of StateEdsOf), and it is
   * compared field by field in what Vulkan looks at. On a difference dynamic state switches off for the
   * rest of the run (both phases) and false is returned: the draw has to use its usual pipeline. Limit: it cannot
   * see the GPU, only that the computations and the record of what was recorded agree.
   */
  bool CheckStateDynamic(const PipelineKey& key, uint64_t n) {
    ++eds_checked_;
    PipelineFixedState fixed;
    FillStateFixed(key, fixed);
    const EdsState& g = eds_recorded_;
    const char* field = nullptr;
    uint32_t pinned = 0;
    uint32_t expected = 0;
    const auto look = [&](const char* label, uint32_t a, uint32_t b) {
      if (!field && a != b) {
        field = label;
        pinned = a;
        expected = b;
      }
    };
    if (eds_mode_ & kEds12) {
      const VkPipelineRasterizationStateCreateInfo& r = fixed.rasterization;
      const VkPipelineDepthStencilStateCreateInfo& z = fixed.depth;
      look("cullMode", g.face, r.cullMode);
      look("frontFace", g.frontface, r.frontFace);
      look("topology", g.topology, fixed.assembly.topology);
      look("primitiveRestartEnable", g.restart, fixed.assembly.primitiveRestartEnable);
      look("depthBiasEnable", g.bias, r.depthBiasEnable);
      look("depthTestEnable", g.test_z, z.depthTestEnable);
      look("depthWriteEnable", g.writes_z, z.depthWriteEnable);
      if (z.depthTestEnable) {
        look("depthCompareOp", g.function_z, z.depthCompareOp);
      }
      look("stencilTestEnable", g.has_stencil, z.stencilTestEnable);
      if (z.stencilTestEnable) {
        look("front.failOp", g.ahead[0], z.front.failOp);
        look("front.passOp", g.ahead[1], z.front.passOp);
        look("front.depthFailOp", g.ahead[2], z.front.depthFailOp);
        look("front.compareOp", g.ahead[3], z.front.compareOp);
        look("back.failOp", g.behind[0], z.back.failOp);
        look("back.passOp", g.behind[1], z.back.passOp);
        look("back.depthFailOp", g.behind[2], z.back.depthFailOp);
        look("back.compareOp", g.behind[3], z.back.compareOp);
      }
    }
    if ((eds_mode_ & kEds3) && fixed.n_colors) {  // phase 2 (without color targets there is nothing to check)
      look("attachmentCount", g.n_colors, fixed.n_colors);
      for (uint32_t a = 0; a < fixed.n_colors && a < 4; ++a) {
        const VkPipelineColorBlendAttachmentState& s = fixed.blends[a];
        const gpu::BlendEquation& q = g.equation[a];
        look("colorWriteMask", g.mask[a], s.colorWriteMask);
        look("blendEnable", g.blend_active[a], s.blendEnable);
        if (s.blendEnable && (s.colorWriteMask & 0x7)) {
          look("srcColorBlendFactor", q.src_color, s.srcColorBlendFactor);
          look("dstColorBlendFactor", q.dst_color, s.dstColorBlendFactor);
          look("colorBlendOp", q.color_op, s.colorBlendOp);
        }
        if (s.blendEnable && (s.colorWriteMask & 0x8)) {
          look("srcAlphaBlendFactor", q.src_alpha, s.srcAlphaBlendFactor);
          look("dstAlphaBlendFactor", q.dst_alpha, s.dstAlphaBlendFactor);
          look("alphaBlendOp", q.alpha_op, s.alphaBlendOp);
        }
      }
    }
    if (field) {
      eds_off_ = true;
      eds_mode_ = 0;
      eds_valid_ = false;
      pipeline_bound_ = VK_NULL_HANDLE;
      REXLOG_ERROR("[native] C6 dynamic state: MISMATCH in {} (check {}: set {} and the usual pipeline would carry {}; "
                   "VS {} PS {}, topology {}, depth {:08X}, rasterization {:02X}, masks {:04X}). Off for the rest of "
                   "the session: the pipelines with all the fixed state come back",
                   field, n, pinned, expected, key.vs, key.ps, key.topology, key.depth,
                   key.rasterization, key.masks);
      return false;
    }
    if (n == kEdsACheck) {
      BO2ZM_REPORT_RING("[native] C6 dynamic state: {} key changes checked against the state of their usual pipeline, 0 "
                           "mismatches; still checking 1 in 4096",
                           n);
    }
    return true;
  }

  /*
   * Phases 1 and 2, before every draw with dynamic state. With the same raw key as the last state set in
   * this buffer there is nothing to do; otherwise what changed is set and the guard checks it for the
   * first 200,000 changes and then 1 in 4,096. Returns false if the guard has switched dynamic state off.
   */
  bool SetStateDynamic(VkCommandBuffer cmd, const PipelineKey& key) {
    ++eds_draws_;
    if (eds_valid_ && std::memcmp(&key, &eds_key_, sizeof(key)) == 0) {
      ++eds_repeated_;
      return true;
    }
    EdsState d;
    StateEdsOf(key, d, eds_mode_);
    EmitStateDynamic(cmd, d, !eds_valid_, eds_mode_);
    eds_key_ = key;
    eds_valid_ = true;
    const uint64_t n = ++eds_checkable_;
    if (n <= kEdsACheck || (n & 4095) == 0) {
      return CheckStateDynamic(key, n);
    }
    return true;
  }

  /*
   * Dynamic state phases 1 and 2. The EDS1/EDS2 vkCmdSet* functions are core in Vulkan 1.3 and the SDK's
   * table does not load them: they are requested from the driver, like vkCmdCopyImage. With an API below
   * 1.3 or with any of them missing, phase 1 is not used at all and one log line says so.
   */
  void LoadStateDynamic() {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    // Through deferred::Wrap: on the job buffer these are queued like the table's (bo2zm_native_deferred.h).
    const auto request = [&](const char* label) {
      return deferred::Wrap(label, ifn.vkGetDeviceProcAddr(device_, label));
    };
    gpu::vulkan::Recorder::Loaded& f = gpu_.loaded();
    f.cull_mode = reinterpret_cast<PFN_vkCmdSetCullMode>(request("vkCmdSetCullMode"));
    f.front_face = reinterpret_cast<PFN_vkCmdSetFrontFace>(request("vkCmdSetFrontFace"));
    f.topology = reinterpret_cast<PFN_vkCmdSetPrimitiveTopology>(request("vkCmdSetPrimitiveTopology"));
    f.restart = reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(request("vkCmdSetPrimitiveRestartEnable"));
    f.depth_bias = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(request("vkCmdSetDepthBiasEnable"));
    f.depth_test = reinterpret_cast<PFN_vkCmdSetDepthTestEnable>(request("vkCmdSetDepthTestEnable"));
    f.depth_write = reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(request("vkCmdSetDepthWriteEnable"));
    f.depth_compare = reinterpret_cast<PFN_vkCmdSetDepthCompareOp>(request("vkCmdSetDepthCompareOp"));
    f.stencil_test = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(request("vkCmdSetStencilTestEnable"));
    f.stencil_op = reinterpret_cast<PFN_vkCmdSetStencilOp>(request("vkCmdSetStencilOp"));
    const uint32_t api = vulkan_device_->properties().apiVersion;
    eds12_available_ = api >= VK_MAKE_API_VERSION(0, 1, 3, 0) && f.cull_mode && f.front_face && f.topology &&
                        f.restart && f.depth_bias && f.depth_test && f.depth_write && f.depth_compare &&
                        f.stencil_test && f.stencil_op;
    REXLOG_INFO("[native] C6 dynamic state: EDS1/EDS2 {} (device API {}.{}.{})",
                eds12_available_ ? "available (core 1.3)" : "NOT available: phase 1 is not used",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
#if defined(REX_UI_VULKAN_DYNAMIC_STATE3)
    // Phase 2. The driver only provides these three if the SDK enabled the extension; the SDK records the
    // three features in its device properties.
    f.blend_enable = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(request("vkCmdSetColorBlendEnableEXT"));
    f.blend_equation = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(request("vkCmdSetColorBlendEquationEXT"));
    f.color_mask = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(request("vkCmdSetColorWriteMaskEXT"));
    const auto& p3 = vulkan_device_->properties();
    eds3_available_ = vulkan_device_->extensions().ext_EXT_extended_dynamic_state3 &&
                       p3.extendedDynamicState3ColorBlendEnable && p3.extendedDynamicState3ColorBlendEquation &&
                       p3.extendedDynamicState3ColorWriteMask && f.blend_enable && f.blend_equation && f.color_mask;
    REXLOG_INFO("[native] C6 dynamic state: EDS3 (blend, equation and color mask) {}",
                eds3_available_ ? "available"
                                 : "NOT available (the device does not provide it or it is not enabled): phase 2 is not "
                                   "used");
#else
    REXLOG_INFO("[native] C6 dynamic state: EDS3 NOT available: this SDK does not enable "
                "VK_EXT_extended_dynamic_state3 (the SDK's extended dynamic state 3 change is missing); phase 2 is not used");
#endif
  }

  // Every 20 s, dynamic state phases 1 and 2.
  void ReportStateDynamic() {
    const auto now = std::chrono::steady_clock::now();
    if (now - eds_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = eds_report_ == std::chrono::steady_clock::time_point{};
    eds_report_ = now;
    const std::array<uint64_t, kEdsN> n = eds_calls_;
    eds_calls_.fill(0);
    const uint64_t draws = eds_draws_ - eds_draws_previous_;
    const uint64_t repeated = eds_repeated_ - eds_repeated_previous_;
    const uint64_t frames = frame_ - eds_frames_previous_;
    eds_draws_previous_ = eds_draws_;
    eds_repeated_previous_ = eds_repeated_;
    eds_frames_previous_ = frame_;
    if (first || !draws) {
      return;
    }
    uint64_t calls = 0;
    for (size_t k = 0; k < kEdsN; ++k) {
      calls += k == kEdsEverything ? 0 : n[k];
    }
    BO2ZM_REPORT_RING(
        "[native] C6 dynamic state: {}; {} draws in 20 s ({:.0f} per frame; {} repeat the previous key) and "
        "{} vkCmdSet* ({:.3f} per draw; {} times everything: new buffer, sky or a pass that forgets the pipeline) | "
        "cull {}, front face {}, topology {}, restart {}, bias {}, Z test {}, Z write {}, Z function {}, stencil {}, "
        "stencil ops {} | blend {}, equation {}, masks {} | pipelines created: {} usual, {} EDS1/EDS2, {} EDS3, {} with "
        "both | guard: {} checked ({})",
        eds_off_ ? "OFF by the guard" : NameModeEds(eds_mode_), draws,
        frames ? double(draws) / double(frames) : 0.0, repeated, calls,
        double(calls) / double(draws), n[kEdsEverything], n[kEdsFace], n[kEdsFrontface], n[kEdsTopology],
        n[kEdsRestart], n[kEdsBias], n[kEdsTestZ], n[kEdsWritesZ], n[kEdsFunctionZ], n[kEdsStencil],
        n[kEdsStencilOps], n[kEdsBlendActive], n[kEdsEquation], n[kEdsMask], pipelines_per_mode_[0],
        pipelines_per_mode_[kEds12], pipelines_per_mode_[kEds3], pipelines_per_mode_[kEds12 | kEds3],
        eds_checked_, eds_checked_ >= kEdsACheck ? "guard passed" : "checking");
  }

  // Phase 2. The blend operation of a register field, like the lambda in FillStateFixed.
  VkBlendOp OperationBlend(uint32_t op) {
    switch (op) {
      case 1:
        return VK_BLEND_OP_SUBTRACT;
      case 2:
        return VK_BLEND_OP_MIN;
      case 3:
        return VK_BLEND_OP_MAX;
      case 4:
        Warn(52, "blend with reverse subtract: subtract is used");
        return VK_BLEND_OP_SUBTRACT;
      default:
        return VK_BLEND_OP_ADD;
    }
  }

  VkPipeline PipelineOf(const PipelineKey& key, const VertexInput& input,
                        const DrawRequest& p) {
    /*
     * One-entry shortcut.
     *
     * There are about 105 pipelines in a whole match and 2,345 draws per frame, so consecutive draws almost always
     * repeat the key. The full lookup is an XXH3 of 80 bytes + an integer division by libstdc++'s prime
     * bucket count + two pointer hops (bucket and node, ~96 B) = 2-3 cache misses, all to end up comparing
     * the same 80 bytes this compares. Here there is just one memcmp on memory that is already hot.
     */
    if (last_key_valid_ && std::memcmp(&last_key_, &key, sizeof(key)) == 0) {
      return last_pipeline_;
    }
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));
    // The direct-mapped cache (bo2zm_native_direct_pipelines) before the map.
    PipelineCell& cell = cells_pipeline_[hash & (kCellsPipeline - 1)];
    if (pipelines_direct_ && cell.pipeline != VK_NULL_HANDLE &&
        std::memcmp(&cell.key, &key, sizeof(key)) == 0) {
      const uint64_t n = ++pipelines_direct_hits_;
      if (!(n <= kPipelinesACheck || (n & 4095) == 0) ||
          CheckCellPipeline(hash, key, cell.pipeline, n)) {
        last_key_ = key;
        last_pipeline_ = cell.pipeline;
        last_key_valid_ = true;
        return cell.pipeline;
      }
      // the guard saw a difference: this draw goes through the map
    }
    if (pipelines_direct_) {
      ++pipelines_direct_misses_;
    }
    if (const auto it = pipelines_.find(hash); it != pipelines_.end()) {
      if (std::memcmp(&it->second.first, &key, sizeof(key)) == 0) {
        last_key_ = key;
        last_pipeline_ = it->second.second;
        last_key_valid_ = true;
        if (pipelines_direct_) {  // the slot keeps the map's pair
          cell.key = key;
          cell.pipeline = it->second.second;
        }
        return it->second.second;
      }
      Warn(50, "pipeline hash collision");
      return VK_NULL_HANDLE;
    }
    // Compiling on the compile thread (see CompilePipelinesLoop): this draw is skipped.
    if (!compile_pending_.empty()) {
      if (const auto pending = compile_pending_.find(hash); pending != compile_pending_.end()) {
        ++pending->second.skipped;
        ++draws_waiting_pipeline_;
        if (!pending->second.first && compile_first_ && CompileFirst(p)) {
          pending->second.first = true;
          PromoteCompile(hash);
        }
        return VK_NULL_HANDLE;
      }
    }
    const VkShaderModule vs = ModuleOf(*p.vs);
    VkShaderModule ps = VK_NULL_HANDLE;
    if (key.ps && bo2zm::native::DebugMagenta()) {  // diagnostics
      ps = ModuleVariant(3, kSpirvBo2zmMagenta, sizeof(kSpirvBo2zmMagenta));
    } else if (key.ps && (key.specialization & kSpecGlowNatural)) {
      ps = ModuleVariant(1, kSpirvGlowEnergy, sizeof(kSpirvGlowEnergy));
    } else if (key.ps && (key.specialization & kSpecGlowSmooth)) {
      ps = ModuleVariant(2, kSpirvGlowSmooth, sizeof(kSpirvGlowSmooth));
    } else if (key.ps && (key.specialization & kSpecOnlyAlpha)) {
      ps = ModuleOnlyAlpha(*p.ps);
    } else if (key.ps && (key.specialization & kSpecZEarly)) {
      ps = ModuleZEarly(*p.ps);
    } else if (key.ps) {
      ps = ModuleOf(*p.ps);
    }
    if (vs == VK_NULL_HANDLE || (key.ps && ps == VK_NULL_HANDLE)) {
      Reject(51, "could not create a shader module");
      return VK_NULL_HANDLE;
    }
    // The VkGraphicsPipelineCreateInfo lives in CreatePipelineVulkan, the same function the prewarm uses from
    // its thread; what surrounds it stays here, as it was.
    uint32_t n_colors = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto start_creation = std::chrono::steady_clock::now();
    // From the driver's cache only (it answers VK_PIPELINE_COMPILE_REQUIRED instead of compiling); one
    // that needs compiling goes to the compile thread and this draw is skipped.
    VkResult created = VK_ERROR_UNKNOWN;
    bool compiled_here = false;  // bo2zm_perf_log: the ring compiled it (not a creation from the driver's cache)
    if (AsyncCompiles()) {
      static const bool all = REXCVAR_GET(bo2zm_native_test_async_all);
      created = all ? VK_PIPELINE_COMPILE_REQUIRED
                    : CreatePipelineVulkan(key, input, vs, ps, pass_rp_, true, pipeline, n_colors,
                                           VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
      if (created == VK_PIPELINE_COMPILE_REQUIRED) {
        QueueCompile(hash, key, input, vs, ps, p);
        ++draws_waiting_pipeline_;
        return VK_NULL_HANDLE;
      }
    } else {
      created = CreatePipelineVulkan(key, input, vs, ps, pass_rp_, true, pipeline, n_colors);
      compiled_here = true;
    }
    if (created != VK_SUCCESS) {
      Reject(53, "could not create a pipeline");
      pipeline = VK_NULL_HANDLE;
    } else {
      ++pipelines_no_save_;
    }
    const uint64_t ns_creation = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - start_creation)
                                              .count());
    ns_pipelines_ += ns_creation;
    if (compiled_here) {
      bo2zm::perf_log::NoteCompile(ns_creation, hash);
    }
    AnnotatePipelineCreated(key, input, p.vs, key.ps ? p.ps : nullptr, pipeline, ns_creation);  // List and measurement
    pipelines_.emplace(hash, std::make_pair(key, pipeline));
    ++pipelines_per_mode_[key.padding2 & 3];  // Dynamic state report
    if (pipelines_direct_ && pipeline != VK_NULL_HANDLE) {  // the same pair as the map
      cell.key = key;
      cell.pipeline = pipeline;
    }
    if (pipelines_.size() <= 64) {
      REXLOG_INFO("[native] C6: pipeline {} (VS n{} PS n{}, topology {}, {} colors, depth {:08X}, blend {:08X}, "
                  "specialization {})",
                  pipelines_.size(), p.vs->number, key.ps ? int(p.ps->number) : -1,
                  key.topology, n_colors,
                  key.depth, key.blend[0], key.specialization);
    }
    return pipeline;
  }

  /*
   * The Vulkan pipeline for a key: PipelineOf's code with the render pass and warn as arguments, so the
   * prewarm and the precompile can create from their threads exactly the same VkGraphicsPipelineCreateInfo
   * as the ring. It only reads its arguments, layout_pipeline_ and the
   * pipeline cache, which Vulkan synchronizes internally; warn = false outside the ring.
   */
  VkResult CreatePipelineVulkan(const PipelineKey& key, const VertexInput& input, VkShaderModule vs,
                               VkShaderModule ps, VkRenderPass pass, bool warn, VkPipeline& pipeline,
                               uint32_t& n_colors_output, VkPipelineCreateFlags create_flags = 0) {
    const VkSpecializationMapEntry map{0, 0, sizeof(uint32_t)};
    const VkSpecializationInfo specialization{1, &map, sizeof(uint32_t), &key.specialization};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) {
      stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage.pName = "main";
      stage.pSpecializationInfo = &specialization;
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;

    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t i = 0; i < input.bindings.size(); ++i) {
      bindings.push_back({i, input.bindings[i].stride, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const VertexAttribute& a : input.attributes) {
      attributes.push_back({a.location, a.binding, a.format, a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertices{};
    vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertices.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vertices.pVertexBindingDescriptions = bindings.data();
    vertices.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertices.pVertexAttributeDescriptions = attributes.data();

    // The fixed state is filled in by FillStateFixed: it is the usual code, moved there without
    // changing any computation, so the guards of the canonical key and of dynamic state compare with what
    // really goes into the VkGraphicsPipelineCreateInfo.
    PipelineFixedState fixed;
    FillStateFixed(key, fixed, warn);  // notify
    const VkPipelineInputAssemblyStateCreateInfo& assembly = fixed.assembly;
    const VkPipelineViewportStateCreateInfo& view = fixed.view;
    const VkPipelineRasterizationStateCreateInfo& rasterization = fixed.rasterization;
    const VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    const VkPipelineDepthStencilStateCreateInfo& depth = fixed.depth;
    const VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    const uint32_t n_colors = fixed.n_colors;

    // Dynamic state phases 1 and 2. Their pipelines (flagged in padding2) also declare as dynamic what is
    // set with vkCmdSet*; their lookup key already has it zeroed (LookupKey).
    VkDynamicState dynamic_states[24] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS};
    uint32_t n_dynamic = 7;
    if (key.padding2 & kEds12) {
      for (const VkDynamicState state : kDynamicEds12) {
        dynamic_states[n_dynamic++] = state;
      }
    }
    if (key.padding2 & kEds3) {
      for (const VkDynamicState state : kDynamicEds3) {
        dynamic_states[n_dynamic++] = state;
      }
    }
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = n_dynamic;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = key.ps ? 2 : 1;  // mode 5: VS only
    info.pStages = stages;
    info.pVertexInputState = &vertices;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &view;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &sampling;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_pipeline_;
    info.renderPass = pass;  // the pass's (ring) or a compatible one (prewarm)
    info.basePipelineIndex = -1;
    info.flags = create_flags;
    n_colors_output = n_colors;
    pipeline = VK_NULL_HANDLE;
    return dfn_.vkCreateGraphicsPipelines(device_, cache_pipelines_, 1, &info, nullptr, &pipeline);
  }

  // --- Pipeline prewarming (bo2zm_native_prewarm_pipelines) ---------------------------
  /*
   * Why. A pipeline that is not in the Vulkan cache has to be compiled: 68-159 ms each on the console. In a
   * first launch after the library and the key had changed there were 56 (5.2 s of stutter at the start, if
   * done on the ring). It happens the first time after any change to the library, the driver or the key,
   * and on a fresh install.
   *
   * How. The ring records every pipeline it creates (AnnotatePipelineCreated) and the list is saved with the
   * cache (SaveCachePipelines). In the next launch, as soon as the library is loaded
   * (TryPrewarm), a lowest-priority thread (LoopPrewarm) walks the list in creation order
   * (the menu first) and recreates each pipeline with CreatePipelineVulkan, the same function the ring
   * uses, with modules from the same SPIR-V (the same variants and fallbacks as PipelineOf) and a
   * compatible render pass (CreatePass, the same formats); then destroys it. The Vulkan cache keeps the
   * compiled shaders, so when the ring asks for it, it comes from the cache (0-2 ms, like the 8 of the
   * menu). The ring changes nothing of what it draws: it only reads two counters of the thread for the
   * report and the guard.
   *
   * Guard. The thread's pipelines are destroyed unused: they cannot change the image. What can fail is
   * that they are not the ones the ring asks for. The report measures how long the ring takes to create
   * the listed pipelines the thread already prewarmed: if at least half of 8 or more take as long as a
   * compiled one (20 ms or more), it writes MISMATCH and stops the thread. Records whose shaders are no
   * longer in the library, or from another dynamic state mode, are skipped.
   */
  enum : uint8_t { kListPending = 0, kListPrewarm = 1, kListNoShader = 2, kListFailed = 3,
                   kListOtherMode = 4 };
  static constexpr uint64_t kNsCompiled = 5000000;  // more than this: really compiled (a cache hit is 0-2 ms)
  static constexpr uint64_t kNsSlow = 20000000;     // the ring creating one already prewarmed: mismatch

  // At start-up (Init): the list of earlier launches, before the thread exists. It is not a
  // separate file: it is the part of the pipelines file that LoadCachePipelines read.
  void LoadListPipelines() {
    const std::filesystem::path path = PathFilePipelines();
    std::vector<uint8_t> data = std::move(list_read_);
    list_read_ = {};
    if (data.size() < kHeaderList ||
        data.size() > kHeaderList + kMaxRegistersList * sizeof(PipelineRecord)) {
      data.clear();
    }
    const char* reason = nullptr;
    uint32_t header[4] = {};
    if (data.empty()) {
      reason = "no list: starting in this session";
    } else {
      std::memcpy(header, data.data(), sizeof(header));
      if (header[0] != kMagicListPipelines || header[1] != kVersionListPipelines ||
          header[2] != sizeof(PipelineRecord) ||
          data.size() != kHeaderList + size_t(header[3]) * sizeof(PipelineRecord)) {
        reason = "list from another version or damaged: starting from scratch";
      }
    }
    if (!reason) {
      list_file_.reserve(header[3]);
      for (uint32_t i = 0; i < header[3]; ++i) {
        PipelineRecord r;
        std::memcpy(&r, data.data() + kHeaderList + size_t(i) * sizeof(PipelineRecord), sizeof(r));
        if (r.n_attributes > PipelineRecord::kMaxAttributes || r.n_bindings > PipelineRecord::kMaxBindings) {
          continue;
        }
        if (index_list_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), list_file_.size()).second) {
          list_file_.push_back(r);
        }
      }
      written_list_ = std::move(data);  // the writer thread rewrites it unchanged if only the cache changes
    }
    LoadExtraListsPipelines();
    LoadPsoList();
    state_list_.assign(list_file_.size(), kListPending);
    REXLOG_INFO("[native] C6 prewarm: {} pipelines in the list of {}{}{}", list_file_.size(),
                path.string(), reason ? ": " : "", reason ? reason : "");
  }

  // The bo2pso text list (bo2zm_pso.psolist beside the executable or in the app bundle, or bo2zm_pso_list): its
  // records join the prewarm list in descending uses, so the precompile builds them behind the loading screen. Its keys
  // are what bo2zm_pso_verify checks the session's pipelines against.
  void LoadPsoList() {
    pso_verify_ = bo2zm::native::pso::Verify();
    const std::filesystem::path path = bo2zm::native::pso::ListToLoad();
    if (path.empty()) {
      if (pso_verify_) {
        REXLOG_WARN("[pso] verify is on but there is no bo2zm_pso.psolist: every pipeline is unlisted");
      }
      return;
    }
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line != "# bo2pso 1") {
      REXLOG_WARN("[pso] {} is not a bo2pso 1 list: ignored", path.string());
      return;
    }
    struct Entry {
      PipelineRecord r;
      uint64_t uses;
    };
    std::vector<Entry> entries;
    uint32_t lines = 0, unusable = 0;
    bool end = false;
    while (std::getline(in, line)) {
      if (line.rfind("# end", 0) == 0) {
        end = true;
        continue;
      }
      if (line.empty() || line[0] != 'G') {
        continue;  // headers, compute pipelines (the native renderer has none)
      }
      ++lines;
      Entry e{};
      if (!PsoRecordOf(line, e.r)) {
        ++unusable;
        continue;
      }
      const size_t tab = line.rfind('\t');
      e.uses = tab == std::string::npos ? 1 : std::strtoull(line.c_str() + tab + 1, nullptr, 10);
      entries.push_back(e);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.uses > b.uses; });
    uint32_t added = 0, known = 0;
    for (const Entry& e : entries) {
      const uint64_t hash = XXH3_64bits(&e.r.key, sizeof(e.r.key));
      pso_listed_.insert(hash);
      if (list_file_.size() >= kMaxRegistersList) {
        continue;
      }
      if (index_list_.emplace(hash, list_file_.size()).second) {
        list_file_.push_back(e.r);
        ++added;
      } else {
        ++known;
      }
    }
    REXLOG_INFO("[pso] {}: {} pipelines, {} new to the prewarm list ({} already listed, {} lines without a record "
                "for this build){}",
                path.string(), lines, added, known, unusable, end ? "" : "; no # end line, the list may be cut short");
  }

  // Ring (AnnotatePipelineCreated): the session's pipelines to the bo2zm_pso_record list, each once, and the
  // bo2zm_pso_verify check.
  void PsoPipelineCreated(const PipelineRecord& r, uint64_t hash, uint64_t ns) {
    if (pso_verify_ && !pso_listed_.count(hash) && pso_unlisted_.insert(hash).second) {
      REXLOG_ERROR("[pso] unlisted pipeline {:016x} ({} ms)", hash, ns / 1000000.0);
      bo2zm::perf_log::Event("compile", fmt::format("key={:016x}\tms={:.2f}\tunlisted=1", hash, ns / 1000000.0));
    }
    if (!pso_record_checked_) {
      pso_record_checked_ = true;
      const std::filesystem::path path = bo2zm::native::pso::RecordPath();
      if (!path.empty()) {
        std::error_code error;
        if (path.has_parent_path()) {
          std::filesystem::create_directories(path.parent_path(), error);
        }
        pso_record_.open(path, std::ios::trunc);
        if (pso_record_) {
          const NativeShaders* library = LibraryActive();
          pso_record_ << "# bo2pso 1\n# build\t" << bo2zm::BuildId() << "\n# renderer\tnative-vulkan\n"
                      << fmt::format("# shaderpack\tbo2zm_shaders.spvpack\t{:016x}\n",
                                     library ? library->Identity() : 0);
          REXLOG_INFO("[pso] recording the session's pipelines to {}", path.string());
        } else {
          REXLOG_WARN("[pso] could not create {}", path.string());
        }
      }
    }
    if (pso_record_.is_open() && pso_recorded_.insert(hash).second) {
      uint64_t key_line = 0;
      pso_record_ << PsoLine(r, key_line) << "\t1\n";
      pso_record_.flush();  // a run stopped by a signal keeps what it saw
    }
  }

  void ClosePsoRecord() {
    if (pso_record_.is_open()) {
      pso_record_ << "# end\tpipelines=" << pso_recorded_.size() << "\n";
      pso_record_.close();
      REXLOG_INFO("[pso] recorded {} pipelines", pso_recorded_.size());
    }
    if (pso_verify_) {
      REXLOG_INFO("[pso] verify: {} unlisted pipelines this session", pso_unlisted_.size());
    }
  }

  // More prewarm lists, cache/prewarm/*.bin, each in the list's own format (tools/pipelines_list.py
  // writes one from a pipelines file). Their new records go after the file's, so the prewarm threads compile them in
  // the menus, before a map that needs them is first visited, and the next save keeps them in the file's list. None
  // is shipped with the program: a list is for players to make from their own pipelines file and share.
  void LoadExtraListsPipelines() {
    const std::filesystem::path folder =
        rex::filesystem::GetExecutableFolder() / std::string(REXCVAR_GET(bo2zm_native_prewarm_lists));
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
      return;
    }
    uint32_t lists = 0, not_lists = 0, added = 0, known = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
      if (!entry.is_regular_file(error) || entry.path().extension() != ".bin") {
        continue;
      }
      std::vector<uint8_t> data;
      ReadFileWhole(entry.path(), data);
      uint32_t header[4] = {};
      if (data.size() >= kHeaderList) {
        std::memcpy(header, data.data(), sizeof(header));
      }
      if (data.size() < kHeaderList || header[0] != kMagicListPipelines || header[1] != kVersionListPipelines ||
          header[2] != sizeof(PipelineRecord) ||
          data.size() != kHeaderList + size_t(header[3]) * sizeof(PipelineRecord)) {
        ++not_lists;
        continue;
      }
      ++lists;
      for (uint32_t i = 0; i < header[3] && list_file_.size() < kMaxRegistersList; ++i) {
        PipelineRecord r;
        std::memcpy(&r, data.data() + kHeaderList + size_t(i) * sizeof(PipelineRecord), sizeof(r));
        if (r.n_attributes > PipelineRecord::kMaxAttributes || r.n_bindings > PipelineRecord::kMaxBindings) {
          continue;
        }
        if (index_list_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), list_file_.size()).second) {
          list_file_.push_back(r);
          ++added;
        } else {
          ++known;
        }
      }
    }
    REXLOG_INFO("[native] C6 prewarm: {} more pipelines from {} lists in {} ({} already listed, {} files that are not "
                "lists)",
                added, lists, folder.string(), known, not_lists);
  }

  // Ring only (SaveCachePipelines): the file's list without the records the thread found missing their
  // shaders, followed by this run's new ones. Above kMaxRegistersList the oldest are dropped.
  std::vector<uint8_t> SerializeListPipelines() const {
    const size_t until = prewarm_until_.load(std::memory_order_acquire);
    std::vector<const PipelineRecord*> registers;
    registers.reserve(list_file_.size() + list_session_.size());
    for (size_t i = 0; i < list_file_.size(); ++i) {
      if (i < until && state_list_[i] == kListNoShader) {
        continue;
      }
      registers.push_back(&list_file_[i]);
    }
    for (const PipelineRecord& r : list_session_) {
      registers.push_back(&r);
    }
    if (registers.size() > kMaxRegistersList) {
      registers.erase(registers.begin(), registers.begin() + std::ptrdiff_t(registers.size() - kMaxRegistersList));
    }
    std::vector<uint8_t> data(kHeaderList + registers.size() * sizeof(PipelineRecord));
    const uint32_t header[4] = {kMagicListPipelines, kVersionListPipelines, uint32_t(sizeof(PipelineRecord)),
                                  uint32_t(registers.size())};
    std::memcpy(data.data(), header, sizeof(header));
    for (size_t i = 0; i < registers.size(); ++i) {
      std::memcpy(data.data() + kHeaderList + i * sizeof(PipelineRecord), registers[i], sizeof(PipelineRecord));
    }
    return data;
  }

  // Ring only (PipelineOf), with every pipeline it creates: the measurement for the report and the guard,
  // and new ones go to the list.
  void AnnotatePipelineCreated(const PipelineKey& key, const VertexInput& input, const ShaderInput* vs,
                               const ShaderInput* ps, VkPipeline pipeline, uint64_t ns) {
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));
    if (pipeline != VK_NULL_HANDLE && vs && vs->shader && (!key.ps || (ps && ps->shader)) &&
        input.attributes.size() <= PipelineRecord::kMaxAttributes &&
        input.bindings.size() <= PipelineRecord::kMaxBindings) {
      PipelineRecord r;
      r.key = key;
      r.hash_vs = vs->shader->hash;
      r.hash_ps = key.ps ? ps->shader->hash : 0;
      r.n_attributes = uint32_t(input.attributes.size());
      for (uint32_t k = 0; k < r.n_attributes; ++k) {
        const VertexAttribute& a = input.attributes[k];
        r.attributes[k] = {a.location, a.binding, uint32_t(a.format), a.offset};
      }
      r.n_bindings = uint32_t(input.bindings.size());
      for (uint32_t k = 0; k < r.n_bindings; ++k) {
        r.strides[k] = input.bindings[k].stride;
      }
      PsoPipelineCreated(r, hash, ns);
    }
    if (const auto it = index_list_.find(hash); it != index_list_.end()) {
      const size_t j = it->second;
      if (j < list_file_.size() && j < prewarm_until_.load(std::memory_order_acquire) &&
          state_list_[j] == kListPrewarm) {
        ++ring_prewarmed_;
        ns_ring_prewarmed_ += ns;
        if (ns >= kNsSlow) {
          ++ring_prewarmed_slow_;
        }
      } else {
        ++list_ring_;
        list_ns_ring_ += ns;
      }
      return;
    }
    ++ring_new_;
    ns_ring_new_ += ns;
    if (pipeline == VK_NULL_HANDLE || !vs || !vs->shader || (key.ps && (!ps || !ps->shader)) ||
        input.attributes.size() > PipelineRecord::kMaxAttributes ||
        input.bindings.size() > PipelineRecord::kMaxBindings ||
        list_file_.size() + list_session_.size() >= kMaxRegistersList) {
      return;
    }
    PipelineRecord r;
    r.key = key;
    r.hash_vs = vs->shader->hash;
    r.hash_ps = key.ps ? ps->shader->hash : 0;
    r.n_attributes = uint32_t(input.attributes.size());
    for (uint32_t k = 0; k < r.n_attributes; ++k) {
      const VertexAttribute& a = input.attributes[k];
      r.attributes[k] = {a.location, a.binding, uint32_t(a.format), a.offset};
    }
    r.n_bindings = uint32_t(input.bindings.size());
    for (uint32_t k = 0; k < r.n_bindings; ++k) {
      r.strides[k] = input.bindings[k].stride;
    }
    index_list_.emplace(hash, SIZE_MAX);  // from this run: not recorded again
    list_session_.push_back(r);
    ++list_no_save_;
    ++ring_a_list_;
  }

  // Ring only, on each submission: starts the thread once, as soon as the library is loaded.
  void TryPrewarm() {
    if (prewarm_decided_) {
      return;
    }
    if (list_file_.empty() || cache_pipelines_ == VK_NULL_HANDLE ||
        !REXCVAR_GET(bo2zm_native_prewarm_pipelines)) {
      prewarm_decided_ = true;
      if (!list_file_.empty()) {
        REXLOG_INFO("[native] C6 prewarm: no prewarming ({})",
                    cache_pipelines_ == VK_NULL_HANDLE ? "no pipeline cache"
                                                       : "bo2zm_native_prewarm_pipelines = false");
      }
      return;
    }
    const NativeShaders* library = LibraryActive();
    if (!library || !library->loaded() || layout_pipeline_ == VK_NULL_HANDLE) {
      return;  // not yet: check again on the next submission
    }
    prewarm_decided_ = true;
    library_prewarm_ = library;
    prewarm_eds_ = eds_mode_;  // the ring would not request those of another dynamic state mode
    prewarm_start_ = std::chrono::steady_clock::now();
    try {
      prewarm_thread_ = std::thread([this] { LoopPrewarm(false); });
      REXLOG_INFO("[native] C6 prewarm: thread created for {} pipelines of the list",
                  list_file_.size());
      if (REXCVAR_GET(bo2zm_native_prewarm_threads) >= 2 && list_file_.size() >= 2) {
        try {
          prewarm_helper_ = std::thread([this] { LoopPrewarm(true); });
        } catch (const std::system_error& error) {
          REXLOG_WARN("[native] C6 prewarm: no second thread ({})", error.what());
        }
      }
    } catch (const std::system_error& error) {
      REXLOG_WARN("[native] C6 prewarm: could not create the thread ({}); no prewarming", error.what());
    }
  }

  // What one prewarm or precompile thread needs to recreate the list's pipelines as the ring would ask for
  // them (LoopPrewarm, Precompile): its own shader modules and render passes, destroyed with it. It only reads the
  // library, the layout and the cache.
  class Warmer {
   public:
    Warmer(VulkanDrawsImpl& draws, const NativeShaders& library) : d_(draws), library_(library) {}
    ~Warmer() {
      for (const auto& [key, shader_module] : modules_) {
        if (shader_module != VK_NULL_HANDLE) {
          d_.dfn_.vkDestroyShaderModule(d_.device_, shader_module, nullptr);
        }
      }
      for (const auto& [key, pass] : passes_) {
        if (pass != VK_NULL_HANDLE) {
          d_.dfn_.vkDestroyRenderPass(d_.device_, pass, nullptr);
        }
      }
    }
    Warmer(const Warmer&) = delete;
    Warmer& operator=(const Warmer&) = delete;

    // Record r's pipeline created, as the ring would ask for it, and destroyed: what stays is the driver's cache.
    // Returns its state; ns, the time the driver took. probe (the precompile): first from the driver's cache alone,
    // as the ring asks for it (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT); compiled then says whether
    // the driver had to compile it. Without probe, compiled stays false (the prewarm tells by the time, kNsCompiled).
    // keep (the precompile, bo2zm_native_precompile_keep): the pipeline is handed back instead of destroyed.
    uint8_t Warm(const PipelineRecord& r, uint64_t& ns, bool probe, bool& compiled, VkPipeline* keep = nullptr) {
      ns = 0;
      compiled = false;
      const ShaderInput* vs = r.key.vs ? library_.PerNumber(r.key.vs - 1) : nullptr;
      const ShaderInput* ps = r.key.ps ? library_.PerNumber(r.key.ps - 1) : nullptr;
      if (!vs || !vs->vertices || !vs->shader || vs->shader->hash != r.hash_vs ||
          (r.key.ps && (!ps || ps->vertices || !ps->shader || ps->shader->hash != r.hash_ps))) {
        return kListNoShader;
      }
      if (r.key.padding2 != d_.prewarm_eds_) {
        return kListOtherMode;
      }
      const VkShaderModule module_vs = Normal(*vs);
      const VkShaderModule module_ps = r.key.ps ? Pixel(r.key, *ps) : VK_NULL_HANDLE;
      const uint64_t key_pass = XXH3_64bits(r.key.formats, sizeof(r.key.formats));
      auto it_pass = passes_.find(key_pass);
      if (it_pass == passes_.end()) {
        it_pass = passes_.emplace(key_pass, d_.CreatePass(r.key.formats, kLoadRead)).first;
      }
      VertexInput input;
      for (uint32_t k = 0; k < r.n_attributes; ++k) {
        const RegisterAttribute& a = r.attributes[k];
        input.attributes.push_back({a.location, a.binding, VkFormat(a.format), a.offset});
      }
      for (uint32_t k = 0; k < r.n_bindings; ++k) {
        input.bindings.push_back({0, r.strides[k]});
      }
      if (module_vs == VK_NULL_HANDLE || (r.key.ps && module_ps == VK_NULL_HANDLE) ||
          it_pass->second == VK_NULL_HANDLE) {
        return kListFailed;
      }
      VkPipeline pipeline = VK_NULL_HANDLE;
      uint32_t n_colors = 0;
      const auto t0 = std::chrono::steady_clock::now();
      VkResult result = VK_PIPELINE_COMPILE_REQUIRED;
      if (probe) {
        result = d_.CreatePipelineVulkan(r.key, input, module_vs, module_ps, it_pass->second, false, pipeline,
                                         n_colors, VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
      }
      if (result == VK_PIPELINE_COMPILE_REQUIRED) {
        compiled = probe;
        pipeline = VK_NULL_HANDLE;
        result = d_.CreatePipelineVulkan(r.key, input, module_vs, module_ps, it_pass->second, false, pipeline,
                                         n_colors);
      }
      ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
      if (result != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
        return kListFailed;
      }
      if (keep) {
        *keep = pipeline;
      } else {
        d_.dfn_.vkDestroyPipeline(d_.device_, pipeline, nullptr);
      }
      return kListPrewarm;
    }

   private:
    VkShaderModule Create(const uint32_t* spirv, size_t bytes) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      VkShaderModule shader_module = VK_NULL_HANDLE;
      if (d_.dfn_.vkCreateShaderModule(d_.device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
      return shader_module;
    }
    // ModuleOf: the library's SPIR-V as is.
    VkShaderModule Normal(const ShaderInput& e) {
      auto it = modules_.find(e.number);
      if (it == modules_.end()) {
        const auto& spirv = bo2zm::shaderlib::Spirv(*e.shader);
        it = modules_.emplace(e.number, Create(spirv.data(), spirv.size() * sizeof(uint32_t))).first;
      }
      return it->second;
    }
    // The pixel shader PipelineOf would choose, in the same order: the bright pass variants
    // (ModuleVariant), without color writes (ModuleOnlyAlpha) and with early tests (ModuleZEarly); the
    // latter two with the same fallback: if it cannot be pruned or patched, the normal one.
    VkShaderModule Pixel(const PipelineKey& key, const ShaderInput& e) {
      const uint32_t spec = key.specialization;
      const uint32_t variant = (spec & kSpecGlowNatural) ? 1
                                : (spec & kSpecGlowSmooth) ? 2
                                : (spec & kSpecOnlyAlpha)        ? 3
                                : (spec & kSpecZEarly)       ? 4
                                                                : 0;
      if (variant == 0) {
        return Normal(e);
      }
      const uint64_t key_module = (uint64_t(variant) << 32) | (variant <= 2 ? 0xFFFFFFFFull : uint64_t(e.number));
      auto it = modules_.find(key_module);
      if (it == modules_.end()) {
        VkShaderModule shader_module = VK_NULL_HANDLE;
        if (variant == 1) {
          shader_module = Create(kSpirvGlowEnergy, sizeof(kSpirvGlowEnergy));
        } else if (variant == 2) {
          shader_module = Create(kSpirvGlowSmooth, sizeof(kSpirvGlowSmooth));
        } else if (variant == 3) {
          uint32_t removed = 0;
          const std::vector<uint32_t> pruned = PruneColorWrites(bo2zm::shaderlib::Spirv(*e.shader), removed);
          if (!pruned.empty()) {
            shader_module = Create(pruned.data(), pruned.size() * sizeof(uint32_t));
          }
        } else {
          const char* reason = "";
          const std::vector<uint32_t> patched = WithTestsEarly(bo2zm::shaderlib::Spirv(*e.shader), reason);
          if (!patched.empty()) {
            shader_module = Create(patched.data(), patched.size() * sizeof(uint32_t));
          }
        }
        it = modules_.emplace(key_module, shader_module).first;
      }
      if (it->second != VK_NULL_HANDLE || variant <= 2) {
        return it->second;
      }
      return Normal(e);
    }

    VulkanDrawsImpl& d_;
    const NativeShaders& library_;
    std::unordered_map<uint64_t, VkShaderModule> modules_;  // (variant << 32) | number
    std::unordered_map<uint64_t, VkRenderPass> passes_;       // by formats
  };

  // The thread. It only reads list_file_, the library, the layout and the cache; it writes
  // state_list_[i] before publishing prewarm_until_ = i + 1, and its atomic counters. Its modules
  // and render pass are its own and it destroys them when done.
  // Helper, the second thread (bo2zm_native_prewarm_threads): it walks the list from its end while no
  // match is on, only into the driver's cache (it writes no state), and stops where the first thread has been.
  void LoopPrewarm(bool helper) {
    rex::thread::set_current_thread_name(helper ? "BO2 pipeline prewarm 2" : "BO2 pipeline prewarm");
    int32_t priority = -1;
#if REX_PLATFORM_SWITCH
    // The lowest priority the system accepts, and never above the guest's (0x3B): compiling at the priority
    // threads are born with would take the core from the game and the ring. If none is accepted, nothing is
    // compiled.
    for (const int32_t candidate : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
      if (RexSwitchSetCurrentThreadPriorityOk(int(candidate))) {
        priority = candidate;
        break;
      }
    }
    if (priority < 0) {
      REXLOG_WARN("[native] C6 prewarm: the system accepts no priority from 0x3B to 0x3F: no prewarming");
      if (!helper) {
        prewarm_finished_.store(true, std::memory_order_release);
      } else {
        prewarm_helper_done_.store(true, std::memory_order_release);
      }
      return;
    }
#endif
    if (!helper) {
      prewarm_priority_.store(priority, std::memory_order_relaxed);
    }
    uint32_t done = 0, compiled = 0, no_shader = 0, other_mode = 0, failed = 0;
    uint64_t ns_compiled = 0, ns_waited = 0;
    const size_t n = list_file_.size();
    {
      Warmer warmer(*this, *library_prewarm_);
      bool unused = false;
      if (helper) {
        for (size_t i = n; i-- > 0;) {
          while (bo2zm::cheats::InMatch() && !prewarm_stop_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ns_waited += 250000000;
          }
          if (prewarm_stop_.load(std::memory_order_relaxed) || i < prewarm_until_.load(std::memory_order_acquire)) {
            break;
          }
          uint64_t ns = 0;
          if (warmer.Warm(list_file_[i], ns, false, unused) == kListPrewarm) {
            ++done;
            if (ns >= kNsCompiled) {
              ++compiled;
              ns_compiled += ns;
            }
          }
        }
      }
      for (size_t i = 0; i < n && !helper; ++i) {
        if (prewarm_stop_.load(std::memory_order_relaxed)) {
          break;
        }
        uint64_t ns = 0;
        const uint8_t state = warmer.Warm(list_file_[i], ns, false, unused);
        if (state == kListNoShader) {
          ++no_shader;
        } else if (state == kListOtherMode) {
          ++other_mode;
        } else if (state == kListPrewarm) {
          ++done;
          if (ns >= kNsCompiled) {
            ++compiled;
            ns_compiled += ns;
          }
        } else {
          ++failed;
        }
        state_list_[i] = state;
        prewarm_done_.store(done, std::memory_order_relaxed);
        prewarm_compiled_.store(compiled, std::memory_order_relaxed);
        prewarm_ns_compiled_.store(ns_compiled, std::memory_order_relaxed);
        prewarm_no_shader_.store(no_shader, std::memory_order_relaxed);
        prewarm_other_mode_.store(other_mode, std::memory_order_relaxed);
        prewarm_failed_.store(failed, std::memory_order_relaxed);
        prewarm_until_.store(i + 1, std::memory_order_release);
      }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarm_start_).count();
    if (helper) {
      // The pipelines it compiled count for the next save (TotalPrewarmCompiled).
      prewarm_compiled_helper_.store(compiled, std::memory_order_relaxed);
      prewarm_helper_done_.store(true, std::memory_order_release);  // for the stamp file
      REXLOG_INFO("[native] C6 prewarm, second thread: {} in {:.1f} s, priority {:#x}: {} pipelines from the list's "
                  "end in the driver's cache, {} really compiled ({:.0f} ms, {:.1f} ms each); {:.1f} s waited for "
                  "matches to end",
                  prewarm_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished", seconds, priority, done,
                  compiled, double(ns_compiled) / 1e6, compiled ? double(ns_compiled) / 1e6 / double(compiled) : 0.0,
                  double(ns_waited) / 1e9);
      return;
    }
    REXLOG_INFO("[native] C6 prewarm: {} in {:.1f} s, priority {:#x}: {} of {} pipelines prewarmed, {} "
                "really compiled ({:.0f} ms, {:.1f} ms each; the rest were already in the cache), {} skipped because "
                "the library no longer has their shaders, {} of another dynamic state mode and {} failed",
                prewarm_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished", seconds, priority,
                done, n, compiled, double(ns_compiled) / 1e6,
                compiled ? double(ns_compiled) / 1e6 / double(compiled) : 0.0, no_shader, other_mode, failed);
    prewarm_finished_.store(true, std::memory_order_release);
  }

  // The precompile at start (bo2zm_native_precompile.h). Ring thread, before the game's first command and
  // with the game held: the list walked as LoopPrewarm walks it, but on every core, above the prewarm's priority, and
  // each pipeline first asked from the driver's cache alone, so that the first one it has to compile brings up the
  // screen. Then the prewarm threads have nothing left to do and are not started.
  void Precompile() override {
    const NativeShaders* library = LibraryActive();
    const size_t n = list_file_.size();
    if (prewarm_decided_ || !library || !library->loaded() || layout_pipeline_ == VK_NULL_HANDLE ||
        cache_pipelines_ == VK_NULL_HANDLE || n == 0) {
      bo2zm::native::precompile::Begin(0, 0);  // nothing to do: the prewarm decides on its own (TryPrewarm)
      return;
    }
    prewarm_decided_ = true;
    library_prewarm_ = library;
    prewarm_eds_ = eds_mode_;
    prewarm_start_ = std::chrono::steady_clock::now();
#if REX_PLATFORM_SWITCH
    // Cores 0-2, one thread each (the system keeps core 3 unless a forwarder gives it).
    const uint32_t threads = uint32_t(std::clamp(bo2zm::CoresInUse(), 1, 3));
#else
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 4u, 10u) - 2;
#endif
    bo2zm::native::precompile::Begin(uint32_t(n), threads);
    // On MoltenVK even a pipeline the caches hold costs a Metal pipeline state (about 1 to 10 ms) when the ring
    // creates it in a match. Kept, the precompile's pipelines are the ring's own and a match creates none of them.
    // Not while recording a pipeline list: that has to see what the session really asks for.
    const bool keep = bo2zm::native::precompile::Keep() && bo2zm::native::pso::RecordPath().empty();
    std::vector<VkPipeline> kept(keep ? n : 0, VK_NULL_HANDLE);
    std::atomic<size_t> next{0};
    std::atomic<uint32_t> done{0}, compiled{0}, no_shader{0}, other_mode{0}, failed{0};
    std::atomic<uint64_t> ns_compiled{0}, ns_cached{0};
    const auto work = [&](uint32_t index) {
      rex::thread::set_current_thread_name("BO2 pipeline precompile");
#if REX_PLATFORM_SWITCH
      // Below the UI thread (0x2C), which draws the screen, and the ring (0x2D); one core each.
      RexSwitchSetCurrentThreadPriorityOk(0x2E);
      RexSwitchSetCurrentThreadCore(int(index % 3));
#else
      (void)index;
#endif
      Warmer warmer(*this, *library);
      for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) {
        uint64_t ns = 0;
        bool was_compiled = false;
        const uint8_t state = warmer.Warm(list_file_[i], ns, true, was_compiled, keep ? &kept[i] : nullptr);
        state_list_[i] = state;  // each record by one thread; read after the join
        if (state == kListNoShader) {
          no_shader.fetch_add(1, std::memory_order_relaxed);
        } else if (state == kListOtherMode) {
          other_mode.fetch_add(1, std::memory_order_relaxed);
        } else if (state == kListPrewarm) {
          done.fetch_add(1, std::memory_order_relaxed);
          if (was_compiled) {
            compiled.fetch_add(1, std::memory_order_relaxed);
            ns_compiled.fetch_add(ns, std::memory_order_relaxed);
          } else {
            ns_cached.fetch_add(ns, std::memory_order_relaxed);
          }
        } else {
          failed.fetch_add(1, std::memory_order_relaxed);
        }
        bo2zm::native::precompile::Advance(was_compiled);
      }
    };
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < threads; ++t) {
      try {
        workers.emplace_back(work, t);
      } catch (const std::system_error& error) {
        REXLOG_WARN("[native] C6 precompile: thread {} not created ({})", t, error.what());
        break;
      }
    }
    if (workers.empty()) {
      work(0);  // no thread at all: on the ring thread itself
    }
    for (std::thread& worker : workers) {
      worker.join();
    }
    // Published as the prewarm thread publishes them: the ring's report and guard read them, and TryPrewarm starts
    // nothing.
    prewarm_done_.store(done.load(), std::memory_order_relaxed);
    prewarm_compiled_.store(compiled.load(), std::memory_order_relaxed);
    prewarm_ns_compiled_.store(ns_compiled.load(), std::memory_order_relaxed);
    prewarm_no_shader_.store(no_shader.load(), std::memory_order_relaxed);
    prewarm_other_mode_.store(other_mode.load(), std::memory_order_relaxed);
    prewarm_failed_.store(failed.load(), std::memory_order_relaxed);
    prewarm_until_.store(n, std::memory_order_release);
    prewarm_finished_.store(true, std::memory_order_release);
    precompiled_ = true;
    // The kept pipelines join the ring's map (this is the ring thread, before its loop), as if it had created them.
    uint32_t adopted = 0;
    for (size_t i = 0; i < kept.size(); ++i) {
      if (kept[i] == VK_NULL_HANDLE) {
        continue;
      }
      const PipelineKey& key = list_file_[i].key;
      if (pipelines_.emplace(XXH3_64bits(&key, sizeof(key)), std::make_pair(key, kept[i])).second) {
        ++adopted;
        ++pipelines_per_mode_[key.padding2 & 3];
      } else {
        dfn_.vkDestroyPipeline(device_, kept[i], nullptr);
      }
    }
    if (keep) {
      REXLOG_INFO("[native] C6 precompile: {} pipelines kept for the ring (bo2zm_native_precompile_keep)", adopted);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarm_start_).count();
    const uint32_t cached = done.load() - compiled.load();
    REXLOG_INFO("[native] C6 precompile: {} pipelines in {:.2f} s on {} threads: {} compiled ({:.0f} ms of the "
                "threads, {:.1f} ms each), {} already in the cache ({:.1f} ms each), {} skipped because the library no "
                "longer has their shaders, {} of another dynamic state mode and {} failed",
                n, seconds, workers.empty() ? size_t(1) : workers.size(), compiled.load(),
                double(ns_compiled.load()) / 1e6,
                compiled.load() ? double(ns_compiled.load()) / 1e6 / double(compiled.load()) : 0.0, cached,
                cached ? double(ns_cached.load()) / 1e6 / double(cached) : 0.0, no_shader.load(),
                other_mode.load(), failed.load());
    // What it compiled goes into the pipelines file now (in slices, while the game starts), and with it the stamp
    // file; a list found whole in the cache only needs the stamp file.
    pipelines_no_save_ += compiled.load();
    prewarm_counted_ = compiled.load();
    if (compiled.load() > 0) {
      SaveCachePipelines(false);
    } else {
      RequestVouch();
    }
  }

  // Ring only, every 10 s if anything changed: the thread's progress, what the ring had to create, and
  // the guard (see above).
  void ReportPrewarm(std::chrono::steady_clock::time_point now) {
    if (now - prewarm_report_ < std::chrono::seconds(10)) {
      return;
    }
    prewarm_report_ = now;
    if (!prewarm_mismatch_ && ring_prewarmed_ >= 8 &&
        ring_prewarmed_slow_ * 2 >= ring_prewarmed_) {
      prewarm_mismatch_ = true;
      prewarm_stop_.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[native] C6 prewarm: MISMATCH: of {} pipelines the ring asked for already prewarmed, {} "
                   "took as long as a compiled one (20 ms or more): what the thread prewarms is not what the ring asks "
                   "for. The thread is stopped; nothing else changes",
                   ring_prewarmed_, ring_prewarmed_slow_);
    }
    const size_t until = prewarm_until_.load(std::memory_order_acquire);
    const bool finished = prewarm_finished_.load(std::memory_order_acquire);
    const uint64_t changes = uint64_t(until) + ring_prewarmed_ + list_ring_ + ring_new_ +
                             (finished ? 1 : 0) + (prewarm_decided_ ? 1 : 0);
    if (changes == prewarm_changes_previous_) {
      return;
    }
    prewarm_changes_previous_ = changes;
    const auto average_of = [](uint64_t ns, uint64_t k) { return k ? double(ns) / double(k) / 1e6 : 0.0; };
    const uint32_t compiled = prewarm_compiled_.load(std::memory_order_relaxed);
    BO2ZM_REPORT_RING(
        "[native] C6 pipeline prewarm: list of {}; thread {} (priority {:#x}): {} walked, {} prewarmed, {} "
        "really compiled ({:.0f} ms), {} without their shaders, {} of another mode, {} failed | the ring created {} of "
        "the list already prewarmed ({:.1f} ms on average, {} slow), {} of the list not prewarmed ({:.1f} ms on "
        "average) and {} new ({:.1f} ms on average; {} added to the list)",
        list_file_.size(),
        precompiled_                ? "not needed (the precompile at start walked the list)"
        : !prewarm_thread_.joinable() ? (prewarm_decided_ ? "not launched" : "waiting for the library")
        : finished                    ? (prewarm_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished")
                                       : "running",
        uint32_t(prewarm_priority_.load(std::memory_order_relaxed)), until,
        prewarm_done_.load(std::memory_order_relaxed), compiled,
        double(prewarm_ns_compiled_.load(std::memory_order_relaxed)) / 1e6,
        prewarm_no_shader_.load(std::memory_order_relaxed), prewarm_other_mode_.load(std::memory_order_relaxed),
        prewarm_failed_.load(std::memory_order_relaxed), ring_prewarmed_,
        average_of(ns_ring_prewarmed_, ring_prewarmed_), ring_prewarmed_slow_, list_ring_,
        average_of(list_ns_ring_, list_ring_), ring_new_, average_of(ns_ring_new_, ring_new_),
        ring_a_list_);
  }

  void StopPrewarm() {
    prewarm_stop_.store(true, std::memory_order_relaxed);
    if (prewarm_thread_.joinable()) {
      prewarm_thread_.join();  // at most, as long as the pipeline being compiled takes
    }
    if (prewarm_helper_.joinable()) {
      prewarm_helper_.join();
    }
    StopCompiles();
  }

  // --- Pipelines compiled on a thread (bo2zm_native_async_pipelines) ----------------------------------------
  /*
   * Why. A pipeline the driver has not compiled before costs 30-200 ms of NAK compilation on the ring, and a new
   * kind of scene asks for dozens at once: switching to HIGH for the first time on a console (shadows, effects, full
   * draw distance and models), compiling on the ring froze the game for 7.5 s and then for 1.4-3.2 s at a time for
   * 20 s on a Switch Lite (the ring's samples in vk_create_graphics_pipeline -> nak_compile_shader).
   *
   * How. The ring asks the driver for the pipeline with VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT
   * (pipelineCreationCacheControl, Vulkan 1.3): one in the cache comes back at once; for the others the
   * driver answers VK_PIPELINE_COMPILE_REQUIRED without compiling. Those go to this thread, which creates them with
   * the same function (the prewarm thread already does: CreatePipelineVulkan is safe from another thread), and their
   * draws are skipped until the ring collects the result at its next frame. The render passes and shader modules a
   * job uses live until the renderer is destroyed, which stops this thread first.
   */
  struct CompileJob {
    uint64_t hash = 0;
    PipelineKey key;
    VertexInput input;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule ps = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    const ShaderInput* vs_input = nullptr;
    const ShaderInput* ps_input = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    uint64_t ns = 0;
    bool first = false;  // bo2zm_native_compile_first: ahead of the others
    // bo2zm_native_test_compile_log: what asked for it, for the log line when it is collected.
    uint32_t id = 0;
    uint32_t category = 0;
    uint64_t frame = 0;
    std::chrono::steady_clock::time_point queued{};
  };

  bool AsyncCompiles() {
    if (async_compiles_ < 0) {
      async_compiles_ = REXCVAR_GET(bo2zm_native_async_pipelines) &&
                                vulkan_device_->properties().pipelineCreationCacheControl
                            ? 1
                            : 0;
      REXLOG_INFO("[native] C6 pipelines compiled on a thread (bo2zm_native_async_pipelines) = {}{}",
                  async_compiles_ ? "YES" : "no",
                  vulkan_device_->properties().pipelineCreationCacheControl ? ""
                                                                             : " (the driver lacks pipelineCreationCacheControl)");
      compile_first_ = uint32_t(REXCVAR_GET(bo2zm_native_compile_first));
      if (async_compiles_) {
        REXLOG_INFO("[native] C6 pipelines that go ahead in the compile queue (bo2zm_native_compile_first): {}",
                    compile_first_ ? fmt::format("{:X}", compile_first_) : std::string("none"));
      }
    }
    return async_compiles_ > 0;
  }

  void QueueCompile(uint64_t hash, const PipelineKey& key, const VertexInput& input, VkShaderModule vs,
                    VkShaderModule ps, const DrawRequest& p) {
    CompileJob job;
    job.hash = hash;
    job.key = key;
    job.input = input;
    job.vs = vs;
    job.ps = ps;
    job.pass = pass_rp_;
    job.vs_input = p.vs;
    job.ps_input = key.ps ? p.ps : nullptr;
    job.id = ++compile_ids_;
    job.category = category_pass_;
    job.frame = frame_;
    job.queued = std::chrono::steady_clock::now();
    job.first = compile_first_ && CompileFirst(p);
    compile_pending_.emplace(hash, PendingCompile{0, job.first});
    static const bool log = REXCVAR_GET(bo2zm_native_test_compile_log);
    size_t waiting = 0;
    size_t place = 0;
    {
      std::lock_guard<std::mutex> lock(compile_mutex_);
      if (job.first) {  // behind the others that go first, ahead of the rest
        place = size_t(std::find_if(compile_queue_.begin(), compile_queue_.end(),
                                    [](const CompileJob& queued) { return !queued.first; }) -
                       compile_queue_.begin());
        compile_queue_.insert(compile_queue_.begin() + ptrdiff_t(place), std::move(job));
      } else {
        place = compile_queue_.size();
        compile_queue_.push_back(std::move(job));
      }
      waiting = compile_queue_.size();
    }
    if (log) {
      LogCompileQueued(compile_ids_, key, p, place);
    }
    // The helper, the first time two or more wait (bo2zm_native_compile_threads).
    if (waiting >= 2 && compile_thread_.joinable() && !compile_helper_.joinable() &&
        REXCVAR_GET(bo2zm_native_compile_threads) >= 2) {
      try {
        compile_helper_ = std::thread([this] { CompilePipelinesLoop(true); });
      } catch (const std::system_error& error) {
        REXLOG_WARN("[native] C6 pipelines compiled on a thread: no second thread ({})", error.what());
      }
    }
    if (!compile_thread_.joinable()) {
      try {
        compile_thread_ = std::thread([this] { CompilePipelinesLoop(false); });
      } catch (const std::system_error& error) {
        REXLOG_ERROR("[native] C6 pipelines compiled on a thread: the thread could not be created ({}); every pipeline "
                     "is compiled on the ring from now on",
                     error.what());
        async_compiles_ = 0;
        std::lock_guard<std::mutex> lock(compile_mutex_);
        for (CompileJob& queued : compile_queue_) {  // compiled here, now
          uint32_t n_colors = 0;
          queued.result = CreatePipelineVulkan(queued.key, queued.input, queued.vs, queued.ps, queued.pass, true,
                                               queued.pipeline, n_colors);
          compile_done_.push_back(std::move(queued));
        }
        compile_queue_.clear();
        return;
      }
    }
    compile_cv_.notify_all();  // the helper's wake-up test differs from the first thread's
  }

  /*
   * bo2zm_native_compile_first (see the cvar): whether this draw's pipeline goes ahead of the others in the compile
   * queue. Its bits are compile_first_. Called only for a draw whose pipeline is compiling or about to, so normal
   * play, with everything compiled, never gets here.
   */
  bool CompileFirst(const DrawRequest& p) const {
    const uint32_t* r = p.registers;
    const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
    return ((compile_first_ & 1) && !pass_formats_[4]) || ((compile_first_ & 2) && pitch >= 1280) ||
           ((compile_first_ & 4) && (r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR] >> 16) <= 6) ||
           ((compile_first_ & 8) && pitch < 640);
  }

  // A queued pipeline that a draw CompileFirst says yes to now asks for: it moves up behind the others that go first
  // (it may already be compiling, or done and not collected yet: then nothing moves).
  void PromoteCompile(uint64_t hash) {
    std::lock_guard<std::mutex> lock(compile_mutex_);
    const auto it = std::find_if(compile_queue_.begin(), compile_queue_.end(),
                                 [hash](const CompileJob& queued) { return queued.hash == hash; });
    if (it == compile_queue_.end()) {
      return;
    }
    CompileJob job = std::move(*it);
    compile_queue_.erase(it);
    job.first = true;
    const auto place = std::find_if(compile_queue_.begin(), compile_queue_.end(),
                                    [](const CompileJob& queued) { return !queued.first; });
    compile_queue_.insert(place, std::move(job));
    ++compiles_promoted_;
  }

  // bo2zm_native_test_compile_log: the draw that sent a pipeline to the compile thread. Category is the pass's
  // (TargetCategory), base the first color target's EDRAM tile, count the draw's index count.
  void LogCompileQueued(uint32_t id, const PipelineKey& key, const DrawRequest& p, size_t place) {
    const uint32_t* r = p.registers;
    const uint32_t initiator = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
    const float scale_x = Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]);
    const float scale_y = Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]);
    REXLOG_INFO("[native] C6 compile queued #{} (frame {}, {} ahead): category {} pass {}x{} pitch {} color base "
                "{:03X} depth {}, VS n{} PS n{}, type {} count {}, viewport {:.0f}x{:.0f}, blend {:08X} depth {:08X}",
                id, frame_, place, category_pass_, pass_width_, pass_height_,
                r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF, r[gr::XE_GPU_REG_RB_COLOR_INFO] & 0xFFF,
                pass_formats_[4] ? "yes" : "no", p.vs->number, key.ps && p.ps ? int(p.ps->number) : -1,
                initiator & 0x3F, initiator >> 16, std::fabs(scale_x) * 2.0f, std::fabs(scale_y) * 2.0f,
                key.blend[0], key.depth);
  }

  // helper: the second thread (bo2zm_native_compile_threads), which only takes a job outside a match and while
  // another one waits behind it, so the first thread always has one.
  void CompilePipelinesLoop(bool helper) {
    rex::thread::set_current_thread_name(helper ? "BO2 pipeline compiles 2" : "BO2 pipeline compiles");
#if REX_PLATFORM_SWITCH
    RexSwitchSetCurrentThreadPriorityOk(0x3B);  // the guest threads' priority: it shares their cores
#endif
    std::unique_lock<std::mutex> lock(compile_mutex_);
    for (;;) {
      compile_cv_.wait(lock, [this, helper] {
        return compile_stop_ ||
               (helper ? compile_queue_.size() >= 2 && !bo2zm::cheats::InMatch() : !compile_queue_.empty());
      });
      if (compile_stop_) {
        return;
      }
      if (helper) {
        compiles_helper_.fetch_add(1, std::memory_order_relaxed);
      }
      CompileJob job = std::move(compile_queue_.front());
      compile_queue_.pop_front();
      lock.unlock();
      const auto start = std::chrono::steady_clock::now();
      uint32_t n_colors = 0;
      job.result = CreatePipelineVulkan(job.key, job.input, job.vs, job.ps, job.pass, false, job.pipeline,
                                          n_colors);  // false: no warnings from this thread, as the prewarm
      job.ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      if (const int32_t delay = REXCVAR_GET(bo2zm_native_test_compile_delay_ms); delay > 0) {
        // A console's pace (test helper), for a pipeline with a shader not compiled before: Mesa keeps the
        // shaders it compiled, so on the console the others come back from the ring at once.
        lock.lock();
        const bool vs_new = compile_modules_seen_.insert(job.vs).second;
        const bool ps_new = job.ps != VK_NULL_HANDLE && compile_modules_seen_.insert(job.ps).second;
        lock.unlock();
        if (vs_new || ps_new) {
          std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
      }
      bo2zm::preset::NoteFirstVisitCompile();  // a map's first visit: the banner, and the game held (bo2zm_cheats)
      lock.lock();
      compile_done_.push_back(std::move(job));
      compiles_ready_.store(true, std::memory_order_release);
    }
  }

  void StopCompiles() {
    {
      std::lock_guard<std::mutex> lock(compile_mutex_);
      compile_stop_ = true;
    }
    compile_cv_.notify_all();
    if (compile_thread_.joinable()) {
      compile_thread_.join();  // at most, as long as the pipeline being compiled takes
    }
    if (compile_helper_.joinable()) {
      compile_helper_.join();
    }
    std::lock_guard<std::mutex> lock(compile_mutex_);
    for (CompileJob& job : compile_done_) {  // finished but never collected
      if (job.pipeline != VK_NULL_HANDLE) {
        dfn_.vkDestroyPipeline(device_, job.pipeline, nullptr);
      }
    }
    compile_done_.clear();
    compile_queue_.clear();
  }

  // Ring, every frame: the pipelines the thread finished go into the map (and the list for the prewarm).
  void CollectCompiledPipelines() {
    if (!compiles_ready_.load(std::memory_order_acquire)) [[likely]] {
      return;
    }
    std::deque<CompileJob> done;
    {
      std::lock_guard<std::mutex> lock(compile_mutex_);
      done.swap(compile_done_);
      compiles_ready_.store(false, std::memory_order_relaxed);
    }
    static const bool log = REXCVAR_GET(bo2zm_native_test_compile_log);
    for (CompileJob& job : done) {
      if (log) {
        const auto pending = compile_pending_.find(job.hash);
        REXLOG_INFO("[native] C6 compile collected #{} (queued in frame {}, category {}{}): {:.0f} ms after it "
                    "was queued ({:.0f} ms compiling), {} draws skipped meanwhile; {} still pending",
                    job.id, job.frame, job.category, job.first ? ", first" : "",
                    double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                 job.queued)
                               .count()) /
                        1e3,
                    double(job.ns) / 1e6, pending != compile_pending_.end() ? pending->second.skipped : 0u,
                    compile_pending_.size() - 1);
      }
      compile_pending_.erase(job.hash);
      if (job.result != VK_SUCCESS) {
        Reject(53, "could not create a pipeline");
        job.pipeline = VK_NULL_HANDLE;
      } else {
        ++pipelines_no_save_;
      }
      ++compiles_async_;
      compiles_first_ += job.first ? 1 : 0;
      ns_compiles_async_ += job.ns;
      compile_ns_worst_ = std::max(compile_ns_worst_, job.ns);
      ns_pipelines_ += job.ns;
      AnnotatePipelineCreated(job.key, job.input, job.vs_input, job.ps_input, job.pipeline, job.ns);
      pipelines_.emplace(job.hash, std::make_pair(job.key, job.pipeline));
      ++pipelines_per_mode_[job.key.padding2 & 3];
    }
  }

  // Every 10 s when something happened: the background compiles and the draws skipped while they ran.
  void ReportCompiles(std::chrono::steady_clock::time_point now) {
    if (now - compile_report_ < std::chrono::seconds(10)) {
      return;
    }
    compile_report_ = now;
    const uint64_t compiled = compiles_async_ - compiles_reported_;
    const uint64_t waited = draws_waiting_pipeline_ - draws_waiting_reported_;
    if (!compiled && !waited) {
      return;
    }
    const uint64_t ns = ns_compiles_async_ - ns_compiles_reported_;
    compiles_reported_ = compiles_async_;
    draws_waiting_reported_ = draws_waiting_pipeline_;
    ns_compiles_reported_ = ns_compiles_async_;
    const uint32_t helper = compiles_helper_.load(std::memory_order_relaxed);
    BO2ZM_REPORT_RING("[native] C6 pipelines compiled on a thread, last 10 s: {} ({:.1f} ms on average, worst "
                      "{:.1f} ms; {} by the second thread), {} still compiling; {} draws skipped while their pipeline "
                      "compiled; ahead of the queue {} ({} moved up; since start)",
                      compiled, compiled ? double(ns) / 1e6 / double(compiled) : 0.0, double(compile_ns_worst_) / 1e6,
                      helper - compiles_helper_reported_, compile_pending_.size(), waited, compiles_first_,
                      compiles_promoted_);
    compiles_helper_reported_ = helper;
    compile_ns_worst_ = 0;
  }

  void DestroyImage(NativeImage& image) {
    if (image.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, image.image, nullptr);
    // The pool chunk is returned after destroying the image that used it, and in that case `guest_memory` is
    // NULL on purpose: the memory belongs to a shared slab and is not freed on its own.
    if (image.pool_block != 0xFFFFFFFFu) pool_textures_.Free(image.pool_block);
    if (image.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, image.guest_memory, nullptr);
    image = NativeImage{};
  }

  struct Heap {
    uint32_t capacity = 0;
    uint32_t next = 1;
    std::vector<uint32_t> free;
  };
  uint64_t slots_full_ = 0;  // views that found no slot (Warn 35), for the 10 s report
  uint64_t slots_full_previous_ = 0;
  uint32_t SlotsUsed(uint32_t heap) const {
    const auto& h = heaps_[heap];
    return h.next - 1 - uint32_t(std::min<size_t>(h.free.size(), h.next - 1));
  }

  const VulkanDevice* vulkan_device_;
  const VulkanDevice::Functions& dfn_;
  gpu::vulkan::Recorder gpu_;  // the layer's recording calls (bo2zm_gpu_vulkan.h), through dfn_
  VkDevice device_;
  rex::memory::Memory* memory_;
  TargetContext* context_;
  FnBufferAddress address_buffer_ = nullptr;

  VkBuffer upload_ = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory_ = VK_NULL_HANDLE;
  uint32_t upload_type_ = UINT32_MAX;
  VkDeviceSize upload_size_real_ = 0;
  uint8_t* upload_data_ = nullptr;
  VkDeviceAddress upload_address_ = 0;
  VkDeviceSize upload_used_ = 0;
  bool upload_coherent_ = false;
  uint64_t epoch_upload_ = 0;
  uint64_t frame_ = 0;
  // Fast untiling and its guard (see ReadLevel).
  static constexpr uint32_t kLevelsACheck = 200;
  static constexpr uint32_t kCheckOneOfEvery = 64;   // after the first ones, by sampling
  uint64_t levels_seen_ = 0;
  uint64_t orders_seen_ = 0;
  int tiled_fast_ = -1;
  uint32_t levels_checked_ = 0;
  uint32_t orders_checked_ = 0;
  std::vector<uint8_t> check_tiled_;
  // Texture check budget (see PrepareTexture).
  static constexpr uint32_t kPostponementMax = 8;
  int32_t budget_hashes_ = -1;
  uint64_t frame_hashes_ = UINT64_MAX;
  uint64_t bytes_hash_frame_ = 0;
  // Sampled recheck of stable textures (see PrepareTexture and HashSample).
  static constexpr uint64_t kSamplesACheck = 3000;  // the first ones, with both hashes: the guard
  int32_t sampling_hashes_ = -1;      // -1 cvar not read; 0 off; N: 1 in N rechecks is full
  uint64_t samples_checked_ = 0;  // stable rechecks with both hashes, since start-up
  uint64_t samples_with_two_ = 0;  // the same, since the last report line
  uint64_t samples_hits_ = 0;     // decided on the sample alone
  uint64_t samples_different_ = 0;    // the sample changed and the full path followed
  uint64_t bytes_sample_ = 0;
  uint64_t bytes_saved_sample_ = 0;
  std::chrono::steady_clock::time_point report_sampling_{};
  // Vertex copies on a separate thread (EnqueueCopy). Power-of-2 capacity.
  const bool copies_active_ = REXCVAR_GET(bo2zm_native_upload_thread);
  std::array<CopyJob, 8192> copies_{};
  std::atomic<size_t> copies_written_{0};
  // How many copies are done, whether by the thread or the ring (see WaitUploads). Outside
  // WaitUploads they finish in order, and then it is also the index of the first one not done.
  std::atomic<size_t> copies_done_{0};
  bool copies_sleeping_ = false;   // under copies_mutex_: the copy thread sleeps or is about to
  bool copies_waiting_ = false;   // under copies_mutex_: the ring waits in WaitUploads
  std::mutex copies_mutex_;
  std::condition_variable copies_cv_;
  std::condition_variable copies_done_cv_;
  bool copies_stop_ = false;  // with copies_mutex_
  std::thread copies_thread_;
  uint64_t copies_in_line_ = 0;
  uint64_t waits_copies_ = 0;
  uint64_t ns_waiting_copies_ = 0;
  std::atomic<size_t> copies_progress_{0};  // copies done by the thread, one by one (diagnostic)
  std::thread::id copies_producer_;
  bool copies_other_thread_warned_ = false;
  // The ring's help (bo2zm_native_upload_help) and its guard. See WaitUploads.
  const bool copies_help_requested_ = REXCVAR_GET(bo2zm_native_upload_help);
  int32_t copies_phase_ = 0;         // CopyPhase; ring only
  bool copies_no_thread_ = false;    // off after a MISMATCH: copies go on the ring; ring only
  std::atomic<size_t> copies_taken_{0};  // how many were taken, in order, by the thread or the ring (with CAS)
  std::mutex copies_chunk_mutex_;          // held by the thread while it copies what it took (lends priority)
  std::array<std::atomic<uint32_t>, 8192> copies_marks_{};  // index + 1 of the last copy done in each slot
  std::atomic<uint64_t> copies_thread_n_{0};  // copies done by the thread, for the report
  std::atomic<uint64_t> copies_thread_bytes_{0};
  size_t copies_verified_ = 0;  // marks checked up to here; ring only
  uint64_t copies_checked_ = 0;
  uint64_t copies_waits_observing_ = 0;
  uint64_t copies_helped_ = 0;
  uint64_t copies_bytes_helped_ = 0;
  uint64_t copies_ns_helping_ = 0;
  uint64_t copies_mismatches_ = 0;
  struct CopyReportFigures {  // the last 10 s (ReportCopies); ring only
    uint64_t queued = 0, bytes_queued = 0, in_line = 0, bytes_in_line = 0;
    uint64_t helped = 0, bytes_helped = 0, ns_helping = 0, waits_with_help = 0;
    uint64_t waits = 0, no_take = 0, waits_chunk = 0, ns_chunk = 0, ns_chunk_worst = 0;
    uint64_t waits_whole = 0, ns_wait = 0, ns_wait_worst = 0, checked = 0;
  };
  CopyReportFigures copies_inf_{};
  std::chrono::steady_clock::time_point copies_report_{};
  uint64_t copies_thread_n_previous_ = 0;
  uint64_t copies_thread_bytes_previous_ = 0;
  struct UploadBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory guest_memory = VK_NULL_HANDLE;
    VkDeviceSize size_real = 0;
    uint8_t* data = nullptr;
    VkDeviceAddress address = 0;
  };
  // One per work slot (there are 3). With fewer than there are slots, two slots would share a buffer and
  // the CPU would write over what the GPU is still reading.
  std::array<UploadBuffer, kSlotsOfJob> uploads_{};  // upload_* are the current slot's
  // GPU skinning (SkinOnGpu).
  bool skin_on_ = false;
  VkPipelineLayout skin_layout_ = VK_NULL_HANDLE;
  VkPipeline skin_pipeline_ = VK_NULL_HANDLE;
  bool skin_bound_ = false;  // the pipeline is bound in this recording's upload command buffer
  // The job tables, one per upload buffer (kSkinTableBytes each: the job records, then the workgroups).
  VkBuffer skin_table_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory skin_table_memory_ = VK_NULL_HANDLE;
  uint8_t* skin_table_data_ = nullptr;
  VkDeviceAddress skin_table_address_ = 0;
  struct SkinTable {
    uint32_t jobs = 0, groups = 0, dispatched = 0;
  };
  std::array<SkinTable, kSlotsOfJob> skin_tables_{};
  // The input cache (SkinCachedInputs): two pools of kSkinPoolBytes in one buffer.
  VkBuffer skin_pool_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory skin_pool_memory_ = VK_NULL_HANDLE;
  uint8_t* skin_pool_data_ = nullptr;
  VkDeviceAddress skin_pool_address_ = 0;
  struct SkinPool {
    VkDeviceSize used = 0;
    uint64_t generation = 1;
    uint64_t left = 0;  // frame_ when the filling moved to the other pool
    VkDeviceSize dirty_lo = ~VkDeviceSize(0), dirty_hi = 0;
  };
  std::array<SkinPool, 2> skin_pools_{};
  uint32_t skin_pool_ = 0;
  struct SkinCacheEntry {
    uint32_t records_guest = 0;
    uint32_t pool = 0;
    uint64_t generation = 0;
    VkDeviceSize offset = 0;
    uint32_t epoch = 0;
    uint32_t uses = 0;
    uint64_t sum = 0;
  };
  std::unordered_map<uint64_t, SkinCacheEntry> skin_cache_;  // by input vertices, count and kernel
  std::vector<bo2zm::gpu_skin::JobRef> skin_jobs_;
  struct SkinCheck {
    uint32_t upload;     // which upload buffer
    VkDeviceSize gpu;    // the dispatch's output
    VkDeviceSize cpu;    // the CPU kernel's, copied
    uint32_t vertices;
    uint32_t kernel;
    uint32_t physical;
  };
  std::vector<SkinCheck> skin_checks_;
  // Separate shared constants, CPU-cached (bo2zm_native_shared_constants_cache).
  std::array<UploadBuffer, kSlotsOfJob> shared_bufs_{};
  bool shared_separate_ = false;
  bool shared_coherent_ = true;
  uint32_t shared_type_ = 0;
  uint8_t* shared_data_ = nullptr;
  VkDeviceMemory shared_memory_ = VK_NULL_HANDLE;
  VkDeviceSize shared_size_real_ = 0;
  VkDeviceSize shared_used_ = 0;
  uint64_t shared_bytes_published_ = 0;

  std::array<VkDescriptorSetLayout, 4> layouts_{};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  // Set 4, the constants through dynamic UBOs. One set per upload slot (each one is a VkBuffer).
  bool use_ubo_ = false;
  VkDeviceSize alignment_ubo_ = 256;
  VkDescriptorSetLayout layout_ubo_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ubo_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotsOfJob> sets_ubo_{};  // one per work slot
  uint32_t slot_current_ = 0;
  bool ubo_bound_ = false;
  uint32_t slot_ubo_bound_ = UINT32_MAX;
  std::array<uint32_t, 3> offsets_ubo_bound_{};
  // Set 4 by differences (ControlSet4, ReportSet4).
  std::array<uint64_t, 8> set4_changes_{};  // binds by what changes: bit 0 VS, bit 1 PS, bit 2 shared
  uint64_t set4_first_ = 0;              // binds after a new command buffer, a new slot or the sky
  uint64_t set4_draws_ = 0;
  std::array<uint64_t, 3> reuploads_vs_{};  // constants uploaded again: by generation, by epoch, because they grow
  std::array<uint64_t, 3> reuploads_ps_{};
  std::chrono::steady_clock::time_point set4_report_{};
  std::chrono::steady_clock::time_point set4_start_alternation_ = std::chrono::steady_clock::now();
  bool set4_requested_ = true;
  bool set4_requested_annotated_ = false;
#if REX_PLATFORM_SWITCH
  bool set4_mismatch_warned_ = false;
  uint64_t set4_nvk_previous_[9] = {};  // NVK counts at the previous report
#endif
  // The draw path in NVK (ControlDrawNvk and ReportDrawNvk). Ring only.
  bool nvk_prefetch_app_ = false;  // hand the pipeline to NVK after PipelineOf
  bool nvk_draw_annotated_ = false;
  bool nvk_improvements_requested_ = true;
  bool nvk_off_warned_[5] = {};
  // ControlCopiesNvk and ReportCopiesNvk.
  bool copies_nvk_annotated_ = false;
  bool copies_nvk_hashes_ = false;
  int32_t copies_nvk_engine_ = -1;
  bool copies_nvk_mismatch_warned_ = false;
  std::chrono::steady_clock::time_point copies_nvk_report_{};
  uint64_t copies_nvk_state_previous_[7] = {};
  uint64_t copies_nvk_copies_previous_[6] = {};
  // ControlKindViewsNvk.
  bool kind_views_annotated_ = false;
  bool kind_views_on_ = false;
  std::chrono::steady_clock::time_point kind_views_report_{};
  uint64_t kind_views_previous_[6] = {};
  std::chrono::steady_clock::time_point nvk_draw_start_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point nvk_report_{};
  uint64_t nvk_partes_previous_[16][2] = {};
  uint64_t nvk_counts_previous_[13] = {};
  uint64_t nvk_improvements_previous_[5][4] = {};
  int32_t alternate_ubo_s_ = 0;
  uint64_t submits_ = 0;
  uint64_t submits_ubo_ = 0;
  std::chrono::steady_clock::time_point start_ubo_ = std::chrono::steady_clock::now();
  std::array<Heap, 4> heaps_{};
  std::array<NativeImage, 3> empty_{};
  VkSampler sampler_empty_ = VK_NULL_HANDLE;
  bool empty_prepared_ = false;

  std::unordered_map<const ShaderInput*, VkShaderModule> modules_;
  std::array<VkShaderModule, 4> modules_variants_{};  // 1 natural, 2 soft, 3 debug magenta
  std::array<bool, 4> modules_variants_created_{};
  int glow_annotated_ = 0;
  std::unordered_map<uint64_t, std::pair<PipelineKey, VkPipeline>> pipelines_;
  // One-entry shortcut for PipelineOf (see the comment there).
  PipelineKey last_key_{};
  VkPipeline last_pipeline_ = VK_NULL_HANDLE;
  bool last_key_valid_ = false;
  // direct cache in front of pipelines_ (bo2zm_native_direct_pipelines).
  struct PipelineCell {
    PipelineKey key{};
    VkPipeline pipeline = VK_NULL_HANDLE;  // VK_NULL_HANDLE = empty cell
  };
  static constexpr size_t kCellsPipeline = 256;  // power of 2; 256 x 88 bytes = 22 KB contiguous
  std::array<PipelineCell, kCellsPipeline> cells_pipeline_{};
  bool pipelines_direct_ = true;           // the cvar, once per frame
  bool pipelines_direct_off_ = false;  // the guard saw a difference
  uint64_t pipelines_direct_hits_ = 0;
  uint64_t pipelines_direct_misses_ = 0;
  uint64_t pipelines_direct_checked_ = 0;
  uint64_t pipelines_direct_hits_previous_ = 0;
  uint64_t pipelines_direct_misses_previous_ = 0;
  std::chrono::steady_clock::time_point pipelines_direct_report_{};
  static constexpr uint64_t kPipelinesACheck = 200000;
  // bo2zm_native_count_pipeline_changes (CountChangePipeline and ReportChangesPipeline).
  enum : uint32_t {
    kChangeBindings = 0,        // Draw's vkCmdBindPipeline calls counted
    kChangeAfterPass,           // of those, the first of a pass (BeginPass forgets the bound pipeline)
    kChangeFirst,            // no previous key in the buffer (new buffer or after the deferred sky)
    kChangeIdentical,           // the same key: only after BeginPass
    kChangeShaders,            // another VS or PS
    kChangeInput,            // same shaders, different vertex input
    kChangeFormats,           // same shaders and input, different formats (another pass)
    kChangeSpecAlpha,           // all the above equal; specialization only changes the alpha test (bits 1, 16-18)
    kChangeSpecZEarly,      // ... the alpha test and the early Z (bit 20)
    kChangeSpecOther,           // ... other specialization bits
    kChangeNoEffect,          // state only, in bits PipelineOf does not read: effectively the same pipeline
    kChangeNoEffectAfterPass,  // ... and the first of a pass
    kChangeOnlyState,         // real state-only changes: what dynamic state would avoid
    kChangeStateAfterPass,     // ... and the first of a pass
    kChangeEds12,              // ... without blending, masks or another topology class (EDS1/EDS2, core 1.3)
    kChangeEds12AfterPass,      // ... and the first of a pass
    kFieldTopology,           // per field, on the canonical state (one bind may change several)
    kFieldClassTopology,
    kFieldBlend,
    kFieldMasks,
    kFieldZ,
    kFieldStencil,
    kFieldFace,
    kFieldRestart,
    kFieldBias,
    kChangesN
  };
  std::array<uint64_t, kChangesN> changes_pipeline_{};
  PipelineKey key_bound_{};       // the last key bound in this command buffer
  bool key_bound_valid_ = false;   // false: new buffer, after the deferred sky, or not counting
  bool count_changes_pipeline_ = true;  // the cvar, once per frame (ReportChangesPipeline)
  uint64_t changes_draws_previous_ = 0;
  uint64_t changes_frames_previous_ = 0;
  std::chrono::steady_clock::time_point changes_report_{};
  // Dynamic state, phase 0a (bo2zm_native_canonical_key, LookupKey).
  static constexpr uint64_t kCanonicalACheck = 200000;
  bool key_canonical_ = true;           // the cvar, once per command buffer (UseSlot)
  bool key_canonical_off_ = false;  // the guard saw a difference
  bool canonical_valid_ = false;         // canonical_raw_ -> canonical_result_ is the previous draw's
  PipelineKey canonical_raw_{};
  PipelineKey canonical_result_{};
  uint64_t canonical_changed_ = 0;      // keys Canonicalize changes (not counting consecutive repeats)
  uint64_t canonical_checked_ = 0;
  uint64_t canonical_changed_previous_ = 0;
  uint64_t canonical_pipelines_previous_ = 0;
  std::chrono::steady_clock::time_point canonical_report_{};
  // Dynamic state, phase 0b (bo2zm_native_pipeline_across_passes).
  bool pipeline_between_passes_ = true;  // the cvar, once per command buffer (UseSlot)
  bool pipeline_between_passes_annotated_ = false;
  uint64_t passes_with_pipeline_ = 0;   // passes started with a bound pipeline that is kept
  uint64_t between_passes_passes_previous_ = 0;
  uint64_t between_passes_kept_previous_ = 0;
  std::chrono::steady_clock::time_point between_passes_report_{};
  // Dynamic state phases 1 and 2 (SetStateDynamic, its guard and its report).
  enum : uint32_t { kEds12 = 1, kEds3 = 2 };  // bits of eds_mode_ and of PipelineKey::padding2
  static constexpr VkDynamicState kDynamicEds12[10] = {
      VK_DYNAMIC_STATE_CULL_MODE,           VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,   VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
  static constexpr uint64_t kEdsACheck = 200000;
  enum : uint32_t {
    kEdsFace = 0,
    kEdsFrontface,
    kEdsTopology,
    kEdsRestart,
    kEdsBias,
    kEdsTestZ,
    kEdsWritesZ,
    kEdsFunctionZ,
    kEdsStencil,
    kEdsStencilOps,
    kEdsBlendActive,  // phase 2
    kEdsEquation,      // phase 2
    kEdsMask,       // phase 2
    kEdsEverything,          // times everything is set (not a call)
    kEdsN
  };
  bool eds12_available_ = false;  // API 1.3 and the ten functions
  bool eds_off_ = false;       // the guard saw a difference: for the rest of the run, everything in the pipeline
  bool eds_mode_annotated_ = false;
  uint32_t eds_mode_ = 0;          // kEds12 | kEds3 of this command buffer (UseSlot; the guard sets it to 0)
  bool eds_valid_ = false;        // eds_recorded_ is what is set in this buffer (false: new buffer, sky or pass)
  EdsState eds_recorded_{};
  PipelineKey eds_key_{};      // raw key of the last state set
  uint64_t eds_draws_ = 0;       // draws with dynamic state
  uint64_t eds_repeated_ = 0;     // ... with the same raw key as the previous one: nothing to set or check
  uint64_t eds_checkable_ = 0;  // ... with something to set (what the guard counts)
  uint64_t eds_checked_ = 0;
  uint64_t eds_draws_previous_ = 0;
  uint64_t eds_repeated_previous_ = 0;
  uint64_t eds_frames_previous_ = 0;
  std::array<uint64_t, kEdsN> eds_calls_{};    // vkCmdSet* recorded per state since the last report
  std::array<uint64_t, 4> pipelines_per_mode_{};  // pipelines created per mode (padding2) since start-up
  std::chrono::steady_clock::time_point eds_report_{};
  // Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3).
  static constexpr VkDynamicState kDynamicEds3[3] = {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
  // The same mapping as kFactors in FillStateFixed (fields 2, 3 and 17-31 give ZERO).
  static constexpr VkBlendFactor kFactorsBlend[32] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  bool eds3_available_ = false;  // the SDK enables the extension and its three features; the driver provides the functions
  std::unordered_map<uint64_t, VkRenderPass> passes_;
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
  // bo2zm_native_framebuffers_forget_views. The views of each cached framebuffer (by the same key), the
  // ones retired by the guard, and the counts for the warning.
  std::unordered_map<uint64_t, std::array<VkImageView, 5>> framebuffers_views_;
  std::vector<VkFramebuffer> fb_retired_;
  bool fb_retire_ = false;
  uint64_t fb_views_forgotten_ = 0;
  uint64_t fb_forgotten_ = 0;
  std::unordered_map<uint64_t, CachedTexture> textures_;
  // Measurement only (bo2zm_native_diag_reuse): textures with a hash, by content key (content key ->
  // texture key; see AnnotateContentTexture), and the counts for the 10 s report.
  std::unordered_multimap<uint64_t, uint64_t> live_per_content_;
  uint64_t contents_different_ = 0;
  int32_t diag_reuse_ = -1;  // -1 = cvar not read
  uint64_t reuse_new_ = 0;
  uint64_t reuse_match_ = 0;
  uint64_t reuse_cold_ = 0;
  uint64_t reuse_no_look_ = 0;
  uint64_t reuse_bytes_new_ = 0;
  uint64_t reuse_bytes_match_ = 0;
  uint64_t reuse_bytes_cold_ = 0;
  uint32_t reuse_details_ = 0;
  std::chrono::steady_clock::time_point report_reuse_{};
  std::unordered_map<uint64_t, View> views_;
  // The views_ keys of each image, so InvalidateImages (every resolve and every copy) does not walk all of
  // views_ (that is 2.7 % of the ring thread in gameplay). Maintained together with views_: SlotView inserts,
  // and every removal of a view removes its key.
  std::unordered_map<VkImage, std::vector<uint64_t>> views_per_image_;
  std::unordered_map<uint64_t, std::pair<VkSampler, uint32_t>> samplers_;
  // Diagnostic: filter values already logged (SlotSampler).
  uint32_t aniso_seen_ = 0;
  bool aniso_annotated_ = false;  // bo2zm_native_anisotropic, one line when the first one is created
  uint32_t diag_mip_min_ = 0;  // bo2zm_native_diag_min_mip, read once per frame
  uint32_t preset_mip_min_ = 0;  // world texture detail; guarded UI draws keep the guest's mip range
  // bo2zm_native_shadow_bias_*, read once per frame.
  int32_t shadows_bias_constant_ = 0;
  int32_t shadows_bias_pending_ = 0;
  bool shadows_bias_annotated_ = false;
  uint32_t mip_max_seen_ = 0;
  std::array<bool, 1024> biases_seen_{};
  std::vector<CachedTexture*> textures_a_upload_;
  std::vector<uint8_t> temporal_;
  uint64_t staging_alloc_failures_ = 0;
  std::vector<uint32_t, UninitializedAllocator<uint32_t>> indices_;
  std::vector<uint32_t, UninitializedAllocator<uint32_t>> converted_;
  std::vector<uint16_t, UninitializedAllocator<uint16_t>> indices16_;  // fast path: 16 bits, not converted
  // Indices16 (bo2zm_native_indices_neon). Ring only.
  int32_t indices_neon_ = -1;  // -1 cvar not read, 0 plain loop, 1 NEON
  uint64_t indices_neon_draws_ = 0;
  uint64_t indices_neon_checked_ = 0;
  std::vector<uint16_t, UninitializedAllocator<uint16_t>> indices_neon_test_;
  // Dynamic state already recorded in the current command buffer and pass.
  bool state_recorded_ = false;
  uint64_t push_recorded_[3] = {};
  gpu::Viewport viewport_recorded_{};
  VkRect2D scissor_recorded_{};
  float blend_recorded_[4] = {};
  float bias_recorded_[2] = {};  // depth bias: constant and slope
  float bias_warned_[2] = {};
  uint32_t warnings_bias_ = 0;
  std::string skip_text_;  // bo2zm_native_diag_skip_ps already read
  std::unordered_set<uint32_t> skip_ps_;
  uint32_t stencil_recorded_[2] = {};  // RB_STENCILREFMASK for front and back
  bool stencil_recorded_valid_ = false;
  VkIndexType type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  VkBuffer buffer_indices_recorded_ = VK_NULL_HANDLE;  // with the index cache, the upload buffer or a pool
  // bo2zm_native_index_cache (IndexCacheByWrites): -1 = not read yet; this slot's arm; this draw's pool binding
  // (tagged as the vertex cache's; 0 = none); the 10 s counters.
  int32_t icache_ = -1;
  bool icache_ab_ = false, icache_active_ = false;
  // bo2zm_native_constants_used: -1 = not read yet; this slot's arm; the shader whose registers alone the last VS / PS
  // block holds (nullptr = the whole block was copied).
  int32_t constants_used_ = -1;
  bool constants_used_ab_ = false, constants_used_active_ = false;
  const ShaderInput* constants_vs_shader_ = nullptr;
  const ShaderInput* constants_ps_shader_ = nullptr;
  int32_t prefetch_ = -1;  // bo2zm_native_prefetch_cache_lines: -1 = not read yet
  bool prefetch_ab_ = false, prefetch_lines_ = false;
  VkDeviceSize indices_pool_ = 0;
  uint64_t icache_hits_ = 0, icache_bytes_hit_ = 0, icache_filled_ = 0, icache_bytes_filled_ = 0, icache_changed_ = 0;
  uint64_t icache_full_ = 0, icache_guard_seen_ = 0, icache_checked_ = 0, icache_unstamped_ = 0;
  uint64_t icache_unstamped_interval_ = 0;
  std::chrono::steady_clock::time_point icache_next_{};
  // Shared constants of the previous draw and where they were uploaded.
  // A draw's shared words are built in shared_buffers_[shared_buffer_]; the other holds
  // the previous draw's (what the comparison in Draw looks at).
  static constexpr size_t kSharedBytes = sizeof(uint32_t) * kWordsShared;
  uint32_t shared_buffers_[2][kWordsShared] = {};
  uint32_t shared_buffer_ = 0;
  bool fewer_copies_ = false, shared_swap_ = false;
  uint64_t shared_observed_ = 0;    // C6 report, to decide how to make the block cheaper
  uint64_t shared_changed_ = 0;
  VkDeviceSize shared_offset_ = 0;
  uint64_t shared_epoch_ = UINT64_MAX;

  VertexInput input_;
  const VertexInput* input_current_ = &input_;  // input_ or a cache element
  std::unordered_map<uint64_t, VertexInput> inputs_cache_;
  uint64_t inputs_cache_hits_ = 0;
  // bo2zm_native_input_front: elements of inputs_cache_ by (VS microcode generation, VS).
  struct InputFront {
    uint64_t generation = 0;
    const ShaderInput* vs = nullptr;
    const VertexInput* input = nullptr;
  };
  static constexpr uint32_t kInputFrontBits = 8;
  std::array<InputFront, size_t(1) << kInputFrontBits> inputs_front_{};
  bool inputs_front_on_ = false;
  bool inputs_front_off_ = false;  // a guard check disagreed
  uint64_t inputs_front_hits_ = 0;
  static constexpr uint64_t kInputFrontChecks = 2000;
  uint64_t inputs_front_checks_ = 0;
  uint64_t inputs_front_sampled_ = 0;
  const ShaderInput* input_vs_ = nullptr;
  uint64_t input_generation_ = UINT64_MAX;
  bool input_valid_ = false;

  uint64_t constants_vs_generation_ = UINT64_MAX;
  uint64_t constants_vs_epoch_ = UINT64_MAX;
  VkDeviceSize constants_vs_offset_ = 0;
  uint64_t constants_ps_generation_ = UINT64_MAX;
  uint64_t constants_ps_epoch_ = UINT64_MAX;
  VkDeviceSize constants_ps_offset_ = 0;
  uint32_t constants_vs_bytes_ = 0;
  uint32_t constants_ps_bytes_ = 0;

  bool pass_active_ = false;
  // The game's open occlusion query and the host query counting this pass's span.
  bool occlusion_open_ = false;
  // reflection visibility (see bo2zm_reflection_demand.h). The witness, every ~1 s at 30 FPS.
  static constexpr uint64_t kWitnessEvery = 32;
  bool measure_visibility_ = false;
  bool witness_pending_ = false;
  uint64_t witness_next_ = 0;
  uint32_t query_occlusion_ = UINT32_MAX;
  uint32_t category_pass_ = kGpuOthers;
  bool submitted_after_shadows_ = false;        // once per frame
  // Per-frame copies of the diagnostic cvars (see the comment at the Swap).
  std::string diag_skip_ps_text_;
  bool diag_vertices_repeated_ = false;
  bool diag_stats_draw_ = false;
  uint32_t scissor_test_ = 0;
  std::array<VkBuffer, 16> buffers_vertices_{};
  // Height actually used by each render target, deduced from the scissor. It only grows.
  std::unordered_map<uint32_t, uint32_t> height_useful_;
  bool area_util_ = true;
  // Small per-draw savings. See their cvars.
  bool framing_cache_ = true;             // C1: the cvar, once per frame
  bool framing_cache_off_ = false;    // C1: the guard saw a difference
  bool framing_valid_ = false;
  uint64_t framing_generation_ = 0;
  uint64_t framing_pass_series_ = 0;
  uint64_t pass_series_ = 0;                // incremented on every BeginPass
  gpu::Viewport framing_viewport_{};
  VkRect2D framing_scissor_{};
  float framing_ndc_[4] = {};
  uint32_t framing_empty_ = 0;
  uint64_t framing_hits_ = 0;         // since start-up (the guard)
  uint64_t framing_checked_ = 0;
  uint64_t framing_hits_report_ = 0;
  uint64_t framing_calcs_report_ = 0;
  static constexpr uint64_t kFramingsACheck = 200000;
  bool height_useful_memo_active_ = true;      // C2
  bool height_useful_memo_off_ = false;
  uint32_t* height_useful_memo_ = nullptr;     // height_useful_ element of the last pitch (the map never erases)
  uint32_t height_useful_memo_pitch_ = 0;
  uint64_t height_useful_memo_hits_ = 0;
  uint64_t height_useful_memo_hits_previous_ = 0;
  static constexpr uint64_t kHeightUsefulACheck = 200000;
  bool key_pass_fast_ = true;          // C3
  bool key_pass_fast_off_ = false;
  uint64_t pass_keys_[5] = {};           // the render target bytes that produced pass_key_
  bool pass_keys_valid_ = false;
  uint64_t keys_pass_fast_ = 0;
  uint64_t keys_pass_fast_previous_ = 0;
  static constexpr uint64_t kKeysPassACheck = 200000;
  bool cvars_per_frame_ = true;        // C5
  bool ps_only_alpha_frame_ = true;
  bool no_ps_no_color_frame_ = true;
  std::chrono::steady_clock::time_point minor_report_{};
  bool no_vegetation_ = false;
  // Shadow map vegetation draws discarded early (see the early discard in Draw).
  uint64_t draws_vegetation_soon_ = 0;
  // VerdictVegetation (bo2zm_d3d_game_vegetation).
  bool vegetation_computed_ = false;  // this draw carries the game's verdict and the ring's has been computed
  bool vegetation_ring_ = false;     // what the early discard has to decide
  uint16_t vegetation_flags_ = 0;
  VegetationDetail vegetation_detail_;
  /*
   * The sky draw, saved in full to replay later (bo2zm_native_postponed_sky).
   *
   * This holds everything the vkCmd* calls of that draw need, by copy: handles (which do not change
   * within the pass) and values (which depend on no guest register once copied). The only thing kept "by
   * reference" are the offsets within the upload buffer, and that buffer only moves forward until
   * UseSlot resets it, always after the pass is closed.
   */
  enum : uint32_t {
    kSkyPerBlend = 0,  // a blended draw arrived: that one does read the background color
    kSkyPerNoZ,        // an opaque draw without Z write arrived: it could not cover the sky
    kSkyAtPassEnd,   // the pass closes (fallback path)
    kSkyPerOtherSky,   // a second sky in the same pass: the first one is emitted now
    kSkyReasons
  };
  struct PostponedSky {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;   // the upload one: vertices and indices
    bool uses_ubo = false;
    uint64_t push[3] = {};             // only with constants through pointers
    std::array<uint32_t, 3> offsets_ubo{};
    uint32_t slot_ubo = 0;
    gpu::Viewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    float bias[2] = {};
    bool with_stencil = false;
    uint32_t stencil[2] = {};          // RB_STENCILREFMASK for front and back
    uint32_t n_bindings = 0;
    std::array<VkDeviceSize, 16> offsets_vertices{};
    bool with_indices = false;
    bool indices_16 = false;
    uint32_t indices = 0;
    uint32_t first_index = 0;
    VkBuffer index_buffer = VK_NULL_HANDLE;  // the upload buffer, or the index cache's pool
    uint32_t vmin = 0;
    uint32_t count = 0;               // no indices
    uint32_t ps_more_one = 0;           // for the diagnostic per-draw query
    uint32_t category = 0;
    uint64_t draws_on_postpone = 0;   // position in the pass when it was deferred
    uint32_t eds_mode = 0;  // phases 1 and 2 its pipeline was looked up with (eds_mode_)
    EdsState eds{};        // and the state that pipeline lacks
  };
  PostponedSky sky_{};
  bool sky_pending_ = false;
  bool sky_postpone_on_ = true;  // the cvar, read once per frame
  bool sky_annotated_ = false;
  uint64_t draws_in_pass_ = 0;
  uint64_t sky_seen_ = 0;
  uint64_t sky_seen_previous_ = 0;
  // The ones that pass both marks (480 indices and first of the pass). This is the number to watch: if it
  // is not 1.00 per frame, the criterion does not isolate the dome and nothing should be deferred. The
  // fingerprint alone ("detected") can give several.
  uint64_t sky_candidates_ = 0;
  uint64_t sky_candidates_previous_ = 0;
  /*
   * The self-checking guard.
   *
   * Enabling this blindly can break the image, so the check that matters ("the criterion detects one draw
   * per frame, not seven") runs inside the game itself, before anything is deferred.
   *
   * Phase 0, observing: for the first kSkyFramesTest gameplay frames nothing is deferred (the image
   *   is exactly the non-deferred one) and the candidates in each frame are counted.
   * Phase 1, deferring: only if those frames never had more than one and at least kSkyFramesWithOne
   *   had exactly one. Then yes.
   * Phase 2, off for good: as soon as two are seen in the same frame, or if the test does not come out
   *   clean. It is not retried for the rest of the run.
   *
   * So the worst case of this change is that it does nothing. Breaking the image is not among the
   * possible outcomes.
   */
  enum : uint32_t { kSkyObserving = 0, kSkyPostponing = 1, kSkyDiscarded = 2 };
  uint32_t sky_guard_ = kSkyObserving;
  uint32_t sky_guard_frames_ = 0;
  uint32_t sky_guard_with_one_ = 0;
  uint32_t sky_guard_in_frame_ = 0;
  uint32_t sky_guard_max_ = 0;
  uint64_t skies_postponed_ = 0;
  uint64_t sky_postponed_previous_ = 0;
  uint64_t sky_no_postponable_ = 0;
  uint64_t sky_two_in_pass_ = 0;
  uint64_t sky_lost_ = 0;
  std::array<uint64_t, kSkyReasons> sky_emitted_{};
  std::array<uint64_t, kSkyReasons> sky_emitted_previous_{};
  uint64_t sky_position_sum_ = 0;
  uint64_t sky_position_sum_previous_ = 0;
  uint64_t sky_position_max_ = 0;
  // The last binding made with vkCmdBindVertexBuffers, so it is not repeated.
  std::array<VkDeviceSize, 16> offsets_recorded_{};
  uint32_t bindings_recorded_ = 0;
  uint64_t bindings_saved_ = 0;
  // bo2zm_native_zero_base_vertices (see the cvar) and its guard.
  bool vertices_base_cero_ = true;           // the cvar, read once per frame (UseSlot)
  bool vertices_base_zero_off_ = false;  // the guard saw a difference: the usual binding for the rest of the run
  uint64_t base_zero_checked_ = 0;       // draws on the base-zero path since start-up (the guard)
  uint64_t base_zero_draws_ = 0;           // since the last report: without binding their own offset
  uint64_t base_cero_total_ = 0;             // since the last report: draws recorded with vertices
  uint64_t base_zero_several_bindings_ = 0;
  uint64_t base_zero_misaligned_ = 0;      // one binding, but the dedupe gave a copy off a multiple
  uint64_t base_zero_bindings_recorded_ = 0;  // vkCmdBindVertexBuffers recorded since the last report
  std::chrono::steady_clock::time_point base_zero_report_{};
  static constexpr uint64_t kBaseZeroACheck = 200000;
  std::array<uint64_t, 51> sub_ns_{};
  uint64_t bytes_constants_vs_ = 0, bytes_constants_ps_ = 0;  // the C6 substages line  // C6 substages, measurement only
  std::array<uint64_t, 51> sub_n_{};
  uint64_t sub_samples_ = 0;
  std::chrono::steady_clock::time_point sub_next_{};
  uint64_t submits_after_shadows_ = 0;         // for the report
  uint32_t stats_pass_ = UINT32_MAX;
  uint32_t warnings_occlusion_draw_ = 0;  // diagnostic of the query draws
  std::chrono::steady_clock::time_point last_warning_occlusion_draw_{};
  VkCommandBuffer pass_commands_ = VK_NULL_HANDLE;
  uint64_t pass_key_ = 0;
  uint64_t pass_generation_ = UINT64_MAX;
  uint32_t pass_width_ = 0;
  uint32_t pass_height_ = 0;
  float pass_scale_ = 1.0f;  // 1 except in the scaled shadow map
  uint64_t clears_depth_in_pass_ = 0;  // ZCULL
  std::chrono::steady_clock::time_point start_pass_{};  // pass change breakdown
  uint32_t pass_formats_[5] = {};
  VkRenderPass pass_rp_ = VK_NULL_HANDLE;
  uint64_t recording_generation_ = UINT64_MAX;
  VkPipeline pipeline_bound_ = VK_NULL_HANDLE;
  bool sets_bound_ = false;

  uint32_t diagnostics_ = 0;
  std::unordered_set<uint32_t> warned_vs_;
  uint32_t warnings_indices_ = 0;
  uint32_t warnings_swizzle_ = 0;
  std::unordered_set<uint64_t> diagnosed_;
  uint64_t drawn_ = 0;
  uint64_t drawn_timed_ = 0;  // of those, with the stage stopwatch
  uint32_t stopwatch_counter_ = 0;
  bool time_ = false;  // the current draw carries the stage stopwatch
  // Reflection diagnostic: cubemap refreshes from their resolved faces (report every 10 s).
  uint64_t cubes_refreshed_ = 0;
  uint64_t cubes_refreshed_previous_ = 0;
  uint64_t frame_report_cubes_ = 0;
  std::chrono::steady_clock::time_point report_cubes_{};
  uint64_t rejected_ = 0;
  // 8 stages. Stage 7 is the pass change, split from the pass stage to tell how much of the 3.9 us per
  // draw is the actual change (15 per frame at 162 us) and how much is what is done on every draw.
  std::array<uint64_t, kStagesDraw> stages_ns_{};
  std::unordered_map<uint32_t, uint64_t> causes_;
  uint32_t last_cause_ = 0;
  uint32_t input_cause_ = 0;  // cause of the failed vertex input
  uint64_t uploads_texture_ = 0;
  uint64_t megs_bytes_ = 0;
  uint64_t bytes_textures_ = 0;  // texture images created (C6 report)
  int32_t textures_mb_max_ = 0;   // bo2zm_native_textures_mb_max
  // Large slabs from which textures take chunks, instead of a dedicated allocation per texture (1.9 ms of
  // CPU each on Horizon). Finish() goes in the destructor after the DestroyImage loops, never before.
  TexturePool pool_textures_;
  // Texture bind thread (bo2zm_native_texture_bind_thread). See the LoopBindings block.
  struct BindRequest {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory guest_memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkResult result = VK_NOT_READY;  // written by the thread, under bindings_mutex_
    uint64_t ns = 0;                    // wall time inside vkBindImageMemory, on the thread
  };
  struct TextureInFlight {  // ring thread only
    CachedTexture* texture = nullptr;
    uint64_t ticket = 0;
    VkImage image = VK_NULL_HANDLE;  // the queued one (to remove it from images_in_flight_ and move its views)
    VkResult result = VK_NOT_READY;
    VkFormat format = VK_FORMAT_UNDEFINED;  // the fields below, to recreate it through CreateTexture if the bind fails
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;
    uint32_t depth3d = 0;
    uint32_t levels = 1;
    bool copy = false;  // UploadTexture already left its data in the upload buffer
    VkDeviceSize copy_offset = 0;
    uint64_t copy_epoch = 0;
    VkCommandBuffer copy_commands = VK_NULL_HANDLE;
  };
  struct ViewInFlight {  // ring thread only
    uint64_t key = 0;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t swizzle = 0;
    uint16_t swizzle_host = 0;
    uint32_t heap = 0;
    uint32_t slot = 0;
  };
  static constexpr uint64_t kQueueBindings = 256;  // power of 2
  static constexpr uint64_t kBindingsACheck = 64;
  static constexpr int32_t kBindingsNoDecide = -1;
  static constexpr int32_t kBindingsOff = 0;
  static constexpr int32_t kBindingsObserving = 1;
  static constexpr int32_t kBindingsApplying = 2;
  std::array<BindRequest, kQueueBindings> queue_bindings_{};
  std::mutex bindings_mutex_;
  std::condition_variable bindings_cv_;         // there are requests or a stop request (the thread waits on it)
  std::condition_variable bindings_done_cv_;  // the thread finished one (the ring waits on it)
  uint64_t bindings_requested_ = 0;       // with bindings_mutex_
  uint64_t bindings_done_ = 0;        // with bindings_mutex_
  uint64_t bindings_collected_ = 0;     // with bindings_mutex_
  bool bindings_sleeping_ = false;     // with bindings_mutex_
  bool bindings_waiting_ = false;     // with bindings_mutex_
  bool bindings_stop_ = false;         // with bindings_mutex_
  bool bindings_priority_ok_ = true;   // with bindings_mutex_
  uint64_t ns_bindings_thread_ = 0;       // with bindings_mutex_
  uint64_t ns_binding_worst_ = 0;        // with bindings_mutex_
  uint64_t bindings_queue_full_ = 0;    // with bindings_mutex_
  std::thread bindings_thread_;
  int32_t bindings_priority_ = 0x2D;  // written by the ring before creating the thread
  // Ring thread only:
  int32_t bindings_phase_ = kBindingsNoDecide;
  bool bindings_stuck_ = false;
  bool measuring_creation_ = false;
  bool collecting_bindings_ = false;
  std::vector<TextureInFlight> in_flight_;
  std::vector<ViewInFlight> views_in_flight_;
  std::unordered_set<VkImage> images_in_flight_;
  uint64_t bindings_checked_ = 0;
  uint64_t bindings_thread_total_ = 0;
  uint64_t bindings_failed_ = 0;
  uint64_t views_postponed_ = 0;
  uint64_t copies_postponed_ = 0;
  uint64_t slots_lost_ = 0;
  uint64_t ns_wait_bindings_total_ = 0;
  // The 10 s report (ReportBindings):
  std::chrono::steady_clock::time_point report_bindings_{};
  uint64_t bindings_thread_report_ = 0;
  uint64_t waits_bindings_report_ = 0;
  uint64_t ns_wait_bindings_report_ = 0;
  uint64_t ns_wait_bindings_worst_ = 0;
  uint64_t ns_create_report_ = 0;
  uint64_t created_report_previous_ = 0;
  uint64_t views_postponed_report_ = 0;
  uint64_t copies_postponed_report_ = 0;
  uint64_t ns_bindings_thread_previous_ = 0;
  uint64_t queue_full_previous_ = 0;
  uint32_t reports_no_compensate_ = 0;
  // Texture hash thread (bo2zm_native_texture_hash_thread). See the PlanHash block.
  std::array<HashJob, kQueueHashes> queue_hashes_{};  // a slot belongs to the thread while it is EnHilo
  std::mutex hashes_mutex_;
  std::condition_variable hashes_cv_;         // there are jobs or a stop request (the thread waits on it)
  std::condition_variable hashes_done_cv_;  // the thread finished one (the ring waits on it)
  uint64_t hashes_published_ = 0;   // under hashes_mutex_ (only the ring writes it)
  uint64_t hashes_taken_ = 0;      // under hashes_mutex_: the thread has looked up to here
  uint64_t hashes_done_ = 0;       // with hashes_mutex_
  bool hashes_sleeping_ = false;    // with hashes_mutex_
  bool hashes_waiting_ = false;    // with hashes_mutex_
  bool hashes_stop_ = false;        // with hashes_mutex_
  bool hashes_priority_ok_ = true;  // with hashes_mutex_
  bool hashes_core_ok_ = true;     // with hashes_mutex_
  int hashes_core_real_ = -1;      // under hashes_mutex_: where the thread ran when starting
  std::thread hashes_thread_;
  int32_t hashes_priority_ = 0x2E;  // written by the ring before creating the thread
  int32_t hashes_core_requested_ = -2;
  int32_t hashes_core_ = -1;       // written by the ring before creating the thread
  // The snapshots and the plans: the ring writes them before publishing each job; the thread only reads
  // them.
  std::unique_ptr<uint8_t[]> snapshots_;
  std::unique_ptr<HashRead[]> reads_hash_;
  uint64_t snapshots_bytes_ = 0;
  // Ring thread only:
  int32_t hashes_phase_ = kHashesNoDecide;
  uint64_t hashes_collected_ = 0;
  uint64_t snapshots_used_ = 0;
  uint32_t reads_used_ = 0;
  std::array<HashRead, kReadsPerTexture> reads_plan_{};
  std::vector<HashJob> hashes_planned_;
  uint64_t hashes_planned_pending_ = 0;
  std::vector<HashJob> collected_hash_jobs_;
  std::array<HashComparison, kQueueHashes> comparisons_hash_{};
  const CachedTexture* comparison_texture_ = nullptr;
  uint64_t comparison_sequence_ = 0;
  std::vector<uint8_t> hashes_data_ring_;
  std::vector<uint8_t> hashes_check_ring_;
  bool hashes_fast_off_ring_ = false;
  bool hashes_no_thread_ = false;
  bool hashes_stuck_ = false;
  bool hashes_suspicious_ = false;
  bool collecting_hashes_ = false;
  uint64_t hashes_comparisons_planned_ = 0;
  uint64_t hashes_new_applying_ = 0;
  uint64_t hashes_thread_total_ = 0;
  uint64_t hashes_ring_total_ = 0;
  uint64_t hashes_compared_ = 0;
  uint64_t hashes_no_compare_ = 0;
  uint64_t hashes_discarded_ = 0;
  uint64_t hashes_reuploads_ = 0;
  // The 10 s report (ReportHashes):
  std::chrono::steady_clock::time_point report_hashes_{};
  uint64_t hashes_planned_report_ = 0;
  uint64_t hashes_comparison_report_ = 0;
  uint64_t hashes_thread_report_ = 0;
  uint64_t hashes_ring_report_ = 0;
  uint64_t ns_hashes_thread_report_ = 0;
  uint64_t ns_hash_raw_thread_report_ = 0;
  uint64_t ns_hashes_ring_report_ = 0;
  uint64_t ns_snapshots_report_ = 0;
  uint64_t bytes_snapshots_report_ = 0;
  uint64_t ns_wait_hashes_report_ = 0;
  uint64_t ns_wait_hashes_worst_ = 0;
  uint64_t waits_hashes_report_ = 0;
  uint64_t hashes_no_room_report_ = 0;
  uint32_t reports_no_compensate_hashes_ = 0;
  int32_t test_no_memory_every_ = 0;  // bo2zm_native_test_out_of_memory_every
  uint64_t texture_reserves_ = 0;
  bool releasing_on_out_of_memory_ = false;  // guard against reentry
  uint64_t released_little_a_little_ = 0;
  uint64_t warning_trickle_ = 0;
  // bo2zm_native_evict_by_key: the cold textures gathered by the last scan (frame, key), its frame, whether it
  // found more than it kept, the scans so far, and the keys released this frame.
  std::vector<std::pair<uint64_t, uint64_t>> cold_list_;
  uint64_t cold_scan_frame_ = 0;
  bool cold_scan_full_ = false;
  uint64_t cold_scans_ = 0;
  std::vector<uint64_t> keys_release_;
  uint64_t attempt_eviction_ = 0;
  uint64_t textures_released_ = 0;
  // Diagnostic of why the cache grows: creations per address and last key per shape (address, format and
  // size). Only touched when a texture is created.
  uint64_t textures_created_ = 0;
  uint64_t created_in_address_view_ = 0;
  uint64_t created_same_shape_other_key_ = 0;
  // The address + 1 last seen and the last shape (address, format and size) with its key and words, in fixed
  // tables made at the first texture (see the diagnostic where textures are created).
  static constexpr uint32_t kDiagSlotBits = 15;
  static constexpr size_t kDiagSlots = size_t(1) << kDiagSlotBits;
  struct DiagShape {
    uint64_t shape = 0;  // 0 = empty
    uint64_t key = 0;
    std::array<uint32_t, 5> words{};
  };
  std::vector<uint32_t> diag_addresses_;
  std::vector<DiagShape> diag_shapes_;
  uint64_t keys_incoherent_ = 0;  // texture key guard (PrepareTexture)
  uint32_t warnings_other_key_ = 0;
  // "C6 counters".
  uint64_t passes_begun_ = 0;
  uint64_t submits_full_ = 0;
  uint64_t ns_submits_full_ = 0;
  uint64_t bytes_vertices_ = 0;
  uint64_t bytes_indices_uploaded_ = 0;
  uint64_t samplers_prepared_ = 0;
  uint64_t samplers_cache_ = 0;
  uint64_t ns_passes_ = 0;
  uint64_t ns_vertices_ = 0;
  uint64_t inputs_computed_ = 0;
  uint64_t ns_inputs_ = 0;
  uint64_t inputs_reused_ = 0;
  uint64_t passes_per_generation_ = 0;
  uint64_t passes_per_target_ = 0;
  uint64_t passes_resumed_ = 0;
  uint64_t texels_passes_ = 0;
  std::array<uint64_t, kGpuCategories> texels_per_category_{};
  std::array<uint64_t, kGpuCategories> draws_per_category_{};
  std::chrono::steady_clock::time_point start_alternation_ps_ = std::chrono::steady_clock::now();
  bool alternation_ps_annotated_ = false;
  bool alternation_only_alpha_annotated_ = false;
  bool inv_size_tex_ = false;
  bool pcf_cheap_ = false;                    // a single shadow map sample
  bool pass_overlay_shadow_ = false;            // bo2zm_native_shadow_min, overlay pass
  bool no_blur_frame_ = true;       // bo2zm_native_no_blur, per frame
  bool mip_point_test_ = false;            // Test: trilinear -> bilinear (measurement only)
  // Deduplication of vertex uploads within the frame.
  DedupeVertices dedupe_;
  bool dedupe_active_ = true;
  // bo2zm_native_vertex_cache.
  struct VertexPool {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* data = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize size_real = 0;
    VkDeviceSize used = 0;
    uint64_t frame_left = 0;  // when the cache last stopped filling it (frame_)
    VkDeviceSize dirty_lo = ~VkDeviceSize(0), dirty_hi = 0;  // written in this submission
  };
  struct VertexCacheEntry {
    uint64_t key = 0;        // address, bytes and order (VertexCache); 0 = empty
    uint64_t stamp = 0;      // frame_ of its last use (the set's oldest is replaced)
    uint64_t hash_seen = 0;  // XXH3 | 1 (or ChangeSum | 1) of the guest bytes the last time it was drawn
    uint64_t hash_pool = 0;  // XXH3 | 1 of the bytes of its persistent copy (0: none)
    uint64_t sum_pool = 0;   // ChangeSum | 1 of the same bytes
    bool seen_by_sum = false;  // hash_seen is a ChangeSum
    VkDeviceSize offset = 0;
    uint32_t pool = 0;
    uint32_t generation = 0;  // the copy is valid while vpool_generation_[pool] is this
    uint32_t copy_epoch = 0;  // by writes: the epoch of its pool copy (0 = none)
    uint32_t seen_epoch = 0;  // by writes: the epoch it was last copied as usual
    uint32_t hits = 0;        // by writes: hits since its pool copy (the guard checks the first ones)
    uint16_t min = 0, max = 0;  // an index range's (bo2zm_native_index_cache)
  };
  std::array<VertexPool, 2> vpools_{};
  uint32_t vpool_ = 0;  // the pool being filled
  std::array<uint32_t, 2> vpool_generation_{1, 1};
  static constexpr size_t kVertexCacheSets = 65536;  // x 4 ways x 48 bytes = 12 MB
  std::vector<VertexCacheEntry> vcache_;
  std::vector<uint8_t> vcache_scratch_;
  bool vcache_ready_ = false, vcache_active_ = false, vcache_off_ = false, vcache_coherent_ = false;
  bool vcache_by_writes_ = false;  // bo2zm_native_vertex_cache_by_writes
  // bo2zm_native_texture_by_writes (TextureUnwritten): -1 = not read yet; this slot's arm; the 10 s counters.
  int32_t tex_by_writes_ = -1;
  bool tex_by_writes_ab_ = false, tex_by_writes_active_ = false;
  uint64_t tex_stamp_seen_ = 0, tex_stamp_skipped_ = 0, tex_stamp_guarded_ = 0, tex_stamp_written_ = 0;
  uint64_t tex_unstamped_ = 0, tex_unstamped_interval_ = 0;
  // bo2zm_native_texture_by_writes_young: -1 = not read yet; this slot's arm; young skips and misses.
  int32_t tex_young_ = -1;
  bool tex_young_ab_ = false, tex_young_active_ = false;
  uint64_t tex_stamp_skipped_young_ = 0, tex_unstamped_young_ = 0;
  std::chrono::steady_clock::time_point tex_stamps_next_{};
  // What the last PrepareTexture did (the "PrepareTexture:" C6 substages): 0 returned before the check, 1 skipped
  // by write stamps, 2 postponed by the budget, 3 sample equal, 4 full hash equal, 6 read again, 7 to the hash
  // thread, 8 took a cold texture's image; prepare_new_ (shown as 5) when it created the image.
  uint32_t prepare_outcome_ = 0;
  bool prepare_new_ = false;
  // bo2zm_native_texture_reuse (ReuseByContent): -1 = not read yet; this slot's arm; the 10 s counters.
  int32_t texreuse_ = -1;
  bool texreuse_ab_ = false, texreuse_active_ = false;
  uint64_t texreuse_taken_ = 0, texreuse_bytes_ = 0, texreuse_missed_ = 0, texreuse_taken_total_ = 0;
  std::chrono::steady_clock::time_point texreuse_next_{};
  uint64_t vcache_unstamped_ = 0, vcache_unstamped_interval_ = 0;
  bool vcache_ab_ = false;
  bool vcache_sum_ = true;      // the check is ChangeSum (bo2zm_native_vertex_cache_sum), read per submission
  bool vcache_sum_ab_ = false;  // bo2zm_native_vertex_cache_sum_ab
  uint64_t vcache_hits_ = 0, vcache_bytes_hit_ = 0, vcache_filled_ = 0, vcache_bytes_filled_ = 0;
  uint64_t vcache_changed_ = 0, vcache_bytes_hashed_ = 0, vcache_ns_hash_ = 0, vcache_full_ = 0;
  uint64_t vcache_switches_ = 0, vcache_guard_seen_ = 0, vcache_checked_ = 0, vcache_evicted_ = 0;
  std::chrono::steady_clock::time_point vcache_report_{};
  // The check frames of the vertex dedupe (hashes, see the binding loop) and their guard.
  static constexpr uint64_t kDedupeChecks = 20000;
  bool dedupe_hash_frame_ = false;
  uint64_t dedupe_hits_frame_start_ = 0;
  uint64_t dedupe_checked_ = 0;     // reuses made in check frames
  uint64_t dedupe_discrepancies_seen_ = 0;
  bool dedupe_keep_flush_ = false;  // bo2zm_native_dedupe_keep_on_flush_waits, read once

  // Once per upload slot (UseSlot): counts the last check frame's reuses, picks whether this one hashes, and on a
  // difference goes back to forgetting at every WAIT_REG_MEM.
  void GuardDedupe() {
    if (dedupe_hash_frame_) {
      dedupe_checked_ += dedupe_.hits() - dedupe_hits_frame_start_;
    }
    if (dedupe_.discrepancies() != dedupe_discrepancies_seen_) {
      dedupe_discrepancies_seen_ = dedupe_.discrepancies();
      if (!g_syncs_on_flush_waits.exchange(true, std::memory_order_relaxed)) {
        REXLOG_ERROR("[native] C6 upload dedupe MISMATCH: a reused vertex range's guest bytes changed after {} "
                     "checked reuses; the dedupe forgets at every WAIT_REG_MEM again for the rest of the session",
                     dedupe_checked_);
      }
    }
    dedupe_hash_frame_ = dedupe_keep_flush_ && (dedupe_checked_ < kDedupeChecks || (frame_ & 511) == 0);
    dedupe_hits_frame_start_ = dedupe_.hits();
  }
  // 16-bit index ranges (DedupeIndices, bo2zm_native_dedupe_indices).
  DedupeIndices dedupe_indices_;
  bool dedupe_indices_on_ = true;
  bool dedupe_indices_off_ = false;  // a MISMATCH: off for the rest of the run
  uint32_t dedupe_indices_sync_view_ = 0;
  const DedupeIndices::Entry* indices_known_ = nullptr;  // this draw's entry (index stage to uploads)
  bool indices_scanned_ = true;  // this draw's indices are in indices16_ (false: the scan was skipped)
  const uint8_t* index_data_ = nullptr;
  bool index_flip_ = false;
  uint32_t index_base_ = 0;
  uint32_t index_order_ = 0;
  static constexpr uint64_t kDedupeIndicesChecks = 20000;
  uint64_t dedupe_indices_hits_ = 0;
  uint64_t dedupe_indices_sampled_ = 0;  // reuses past the first kDedupeIndicesChecks (1 in 4,096 checked)
  uint64_t dedupe_indices_checks_ = 0;
  uint64_t dedupe_indices_converted_ = 0;
  uint64_t bytes_indices_saved_ = 0;
  uint64_t dedupe_indices_report_[4] = {};

  // A reuse that still ran the scan (the first kDedupeIndicesChecks, then 1 in 4,096): the scan's minimum and
  // maximum, and the guest's index bytes against the hash taken when the copy was uploaded (when it has one): equal
  // bytes give an equal copy. The upload buffer itself is not read back (on both platforms it is write-combined
  // memory, very slow to read).
  void CheckKnownIndices(uint32_t count, uint32_t min, uint32_t max) {
    ++dedupe_indices_checks_;
    bool same = indices_known_->min == min && indices_known_->max == max;
    if (same && indices_known_->hash) {
      same = XXH3_64bits(index_data_, size_t(count) * 2) == indices_known_->hash;
    }
    if (!same) {
      REXLOG_ERROR("[native] C6 index dedupe MISMATCH after {} checks: range {:08X} ({} indices) had min {} max {}, "
                   "the scan gives {} and {}{}. Off for the rest of the session",
                   dedupe_indices_checks_, index_base_, count, indices_known_->min, indices_known_->max, min, max,
                   indices_known_->min == min && indices_known_->max == max ? " (the uploaded copy differs)" : "");
      dedupe_indices_off_ = true;
      dedupe_indices_on_ = false;
      indices_known_ = nullptr;  // this draw uploads its fresh scan
      return;
    }
    if (dedupe_indices_checks_ == kDedupeIndicesChecks) {
      REXLOG_INFO("[native] C6 index dedupe: {} reuses checked against a fresh scan, 0 differences: applying (1 in "
                  "4,096 still checked)",
                  kDedupeIndicesChecks);
    }
    indices_scanned_ = true;
  }
  uint32_t dedupe_sync_view_ = 0;  // last value seen of g_syncs_ring
  std::set<uint64_t> warnings_invsize_resolved_;  // one trace per size
  bool alternation_scissor_annotated_ = false;
  std::unordered_map<const ShaderInput*, VkShaderModule> modules_only_alpha_;
  // The same module with OpExecutionMode EarlyFragmentTests (bo2zm_native_early_z).
  std::unordered_map<const ShaderInput*, VkShaderModule> modules_z_early_;
  uint64_t draws_ps_useless_ = 0;     // no color and a PS that does not discard
  uint64_t draws_ps_needed_ = 0;  // no color, but the PS is needed
  uint64_t shadows_alpha_active_ = 0;
  uint64_t scene_with_discard_ = 0;
  uint64_t scene_no_discard_ = 0;   // shadow map draws with the alpha test enabled
  /*
   * bo2zm_native_early_z and bo2zm_native_skip_invisible. The first four split C6's "prevent it"
   * in two: those fixed by testing earlier and those that cannot be fixed because they write depth or
   * stencil (the latter are the exact size of a depth pre-pass).
   */
  enum : uint32_t {
    kZSet = 0,       // its depth test is moved earlier
    kZWritesZ,         // not possible: the draw writes depth
    kZStencil,         // not possible: stencil is involved
    kZOcclusion,         // left alone: a game occlusion query is open
    kZAlreadyEarly,       // no alpha test or kill: the GPU already tested early
    kInvisibleBlend,   // blending copies the destination: the draw paints nothing
    kInvisibleAlpha,     // alpha test with the NEVER function
    kInvisibleOnlyColor,  // invisible in color but writes Z: only its color is removed
    kZCounts
  };
  std::array<uint64_t, kZCounts> counts_z_{};
  std::array<uint64_t, kZCounts> counts_z_previous_{};
  uint64_t z_early_no_module_ = 0;
  uint64_t frames_z_ = 0;
  uint64_t frames_z_previous_ = 0;
  std::chrono::steady_clock::time_point last_report_z_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point start_alternation_z_ = std::chrono::steady_clock::now();
  bool z_early_ = true;
  bool alternation_z_annotated_ = false;
  bool skip_invisible_ = true;
  uint64_t shadows_alpha_off_ = 0;  // ... and without it (the stage would be unnecessary)
  std::array<uint64_t, kGpuCategories> triangles_per_category_{};
  std::array<uint64_t, kGpuCategories> passes_per_category_{};
  uint64_t ns_render_pass_ = 0;
  // Test bo2zm_native_skip_shadows_alternate_s.
  std::chrono::steady_clock::time_point start_shadows_ = std::chrono::steady_clock::now();
  bool shadows_skipped_ = false;
  // Diagnostic bo2zm_native_diag_repeated_vertices.
  struct SeenVertices {
    uint64_t frame = UINT64_MAX;
    uint64_t hash = 0;
  };
  std::unordered_map<uint64_t, SeenVertices> vertices_seen_;
  uint64_t bytes_repeated_frame_ = 0;
  uint64_t bytes_equal_earlier_ = 0;
  uint64_t ns_hash_vertices_ = 0;
  // Last texture and sampler result of each PS sampler register.
  struct CacheSampler {
    std::array<uint32_t, 6> fetch{};
    uint64_t frame = UINT64_MAX;
    uint64_t generation = 0;
    uint32_t slot = 0;
    uint32_t heap = 0;
    uint32_t sampler = 0;
    uint32_t mip_min = 0;  // effective floor, including the draw's UI exemption
    uint32_t generation_slot = 0;  // generation_slot_ of its texture slot when it was filled
    uint64_t valid_until = 0;  // last frame it is valid without going back to PrepareTexture
    uint32_t width = 0;  // host image size, for 1/size
    uint32_t height = 0;
  };
  bool cache_between_frames_ = true;  // bo2zm_native_texture_cache_across_frames
  bool mipmaps_ = true;                 // bo2zm_native_mipmaps
  bool diag_mips_ = false;              // bo2zm_native_diag_mips
  uint64_t mips_reviewed_ = 0;
  uint64_t mips_rare_ = 0;
  uint64_t levels_mip_uploaded_ = 0;    // report: mip levels (without the base) in created textures
  std::array<CacheSampler, kSlotsPs + kSlotsVs> cache_samplers_{};  // by slot
  // Second cache, by the whole fetch constant: a direct-mapped table (the hash picks the slot). It went
  // from 256 to 1024 slots because in gameplay there are ~340 distinct textures per frame and 256 slots
  // collided (64 KB: fits in L2).
  /*
   * From 1024 to 4096 (288 KB, still fits in the 2 MB L2). In one heavy stretch there are ~3,000
   * draws per frame and ~205 misses of this table per frame (C6 counters: 97,709 in 20 s), at ~15 us
   * each through PrepareTexture (the texture stage measures 1.2-1.5 us per draw with 3.4 % misses). With
   * N distinct fetches in M slots, the fraction sharing a slot with another is 1 - e^(-N/M): with ~600
   * that is 44 % at 1024 and 14 % at 4096. The hit conditions do not change: there is just more room. The
   * misses-by-cause report (every 10 s) says how many were collisions.
   */
  std::array<CacheSampler, 4096> cache_fetch_{};
  std::array<uint32_t, 4096> cache_fetch_dir_{};  // each entry's AddressCacheSampler
  uint64_t samplers_cache_fetch_ = 0;
  uint64_t fetch_misses_collision_ = 0;      // The slot held another fetch constant
  uint64_t fetch_misses_empty_ = 0;       // cell not used yet
  uint64_t fetch_misses_generation_ = 0;  // same fetch, but generation_textures_ changed
  // What drops sampler cache entries (report "C6 sampler cache by fetch")
  uint64_t invalidations_global_ = 0;   // InvalidateTextures
  uint64_t invalidations_retired_ = 0;  // ReleaseImages and ForgetImage (only the slots of their views)
  uint64_t invalidations_targeted_ = 0;  // InvalidateTexturesIn
  std::array<uint64_t, 3> invalidations_previous_{};
  uint64_t fetch_misses_expired_ = 0;    // same fetch, but the texture is due for a check (valid_until)
  std::array<uint64_t, 9> fetch_report_previous_{};
  std::chrono::steady_clock::time_point fetch_report_{};
  uint64_t generation_textures_ = 0;  // changes with InvalidateTextures and the mip setting
  // The texture words reuse (Draw, textures stage; bo2zm_native_reuse_textures).
  static constexpr uint64_t kReuseTexturesChecks = 20000;
  bool reuse_textures_on_ = true;
  bool reuse_valid_ = false;    // the snapshot below may be reused (its draw's uploads succeeded)
  bool reuse_pending_ = false;  // the snapshot was taken with no invalidation during its loop
  uint32_t reuse_shared_[kWordsShared] = {};
  bool reuse_reads_reflection_ = false;
  const ShaderInput* reuse_ps_ = nullptr;
  uint32_t reuse_mip_min_ = 0;
  uint64_t reuse_generation_fetch_ = 0;
  uint64_t reuse_generation_textures_ = 0;
  uint64_t reuse_frame_ = 0;
  uint64_t reuse_slots_retired_ = 0;
  uint64_t slots_retired_ = 0;  // RetireSlot calls
  uint64_t reuse_checks_ = 0;
  uint64_t reuse_sampled_ = 0;
  uint64_t reuse_hits_ = 0;
  uint64_t reuse_by_value_ = 0;      // reuses whose fetch generation had moved but whose sampler words had not
  uint32_t reuse_fetch_[16 * 6] = {};  // the snapshot's fetch constant slots 0-15

  // The six fetch words of every sampler slot (0-15) the pixel shader reads, as in the snapshot (reuse_fetch_).
  bool SameSamplerFetch(const ShaderInput* ps, const uint32_t* r) const {
    if (!ps) {
      return true;  // no samplers: the loop reads no fetch constant
    }
    for (const SamplerShader& sampler : ps->samplers) {
      if (sampler.reg_index >= 16) {
        continue;
      }
      const uint32_t at = uint32_t(sampler.reg_index) * 6;
      if (std::memcmp(r + kRegFetch + at, reuse_fetch_ + at, 6 * sizeof(uint32_t)) != 0) {
        return false;
      }
    }
    return true;
  }
  /*
   * One generation per texture descriptor slot (2D, 3D and cube heaps). A slot's generation changes when
   * its view is retired (the slot goes back to the free list and will hold another texture) or when the image behind
   * it stops being the one a fetch constant should get (InvalidateImages). A sampler cache entry is only valid with
   * the generation its slot had when it was filled. A single global generation would drop the whole cache every time
   * an image is retired: on the console that is ~6 times per second and 13,000-24,000 table misses per 10 s ("by
   * generation"), each one a trip through PrepareTexture (~15 us on the Switch).
   */
  std::array<std::vector<uint32_t>, 3> generation_slot_;
  uint32_t GenerationSlot(uint32_t heap, uint32_t slot) const {
    return heap < generation_slot_.size() && slot < generation_slot_[heap].size()
               ? generation_slot_[heap][slot]
               : 0;
  }
  void RetireSlot(uint32_t heap, uint32_t slot) {
    if (heap < generation_slot_.size() && slot < generation_slot_[heap].size()) {
      ++generation_slot_[heap][slot];
    }
    ++slots_retired_;  // the texture words reuse (Draw) checks no slot was retired since
  }

  // The guard of the texture words reuse (Draw, textures stage): the loop ran anyway, and its words must
  // be the ones the reuse would have copied.
  void CheckReuseTextures(const uint32_t* shared, bool reads_reflection, const ShaderInput* ps) {
    if (std::memcmp(shared, reuse_shared_, sizeof(reuse_shared_)) == 0 &&
        reads_reflection == reuse_reads_reflection_) {
      ++reuse_checks_;
      if (reuse_checks_ == kReuseTexturesChecks) {
        REXLOG_INFO("[native] C6 texture words reuse: {} reuses checked against the sampler loop, 0 differences: "
                    "APPLYING (1 in 1,024 still checked)",
                    reuse_checks_);
      }
      return;
    }
    uint32_t first = 0;
    while (first < kWordsShared && shared[first] == reuse_shared_[first]) {
      ++first;
    }
    REXLOG_WARN("[native] C6 texture words reuse MISMATCH after {} checks: word {} is {:08X}, the reuse had {:08X} "
                "(reflection {} / {}; PS n{}): reuse off for this session",
                reuse_checks_, first, first < kWordsShared ? shared[first] : 0u,
                first < kWordsShared ? reuse_shared_[first] : 0u, reads_reflection, reuse_reads_reflection_,
                ps ? int(ps->number) : -1);
    reuse_textures_on_ = false;
  }
  bool SlotCurrent(const CacheSampler& c) const {
    return c.generation_slot == GenerationSlot(c.heap, c.slot);
  }
  // On-disk pipeline cache (LoadCachePipelines and SaveCachePipelines).
  using FnCreatePipelineCache = VkResult(VKAPI_PTR*)(VkDevice, const VkPipelineCacheCreateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkPipelineCache*);
  using FnPipelineCacheData = VkResult(VKAPI_PTR*)(VkDevice, VkPipelineCache, size_t*, void*);
  using FnDestroyPipelineCache = void(VKAPI_PTR*)(VkDevice, VkPipelineCache,
                                                    const VkAllocationCallbacks*);
  FnPipelineCacheData data_cache_ = nullptr;
  FnDestroyPipelineCache destroy_cache_ = nullptr;
  VkPipelineCache cache_pipelines_ = VK_NULL_HANDLE;
  uint32_t pipelines_no_save_ = 0;
  uint32_t saved_cache_ = 0;
  size_t bytes_cache_saved_ = 0;  // size of the last file read or saved (the writer thread's once it runs)
  std::chrono::steady_clock::time_point cache_saved_{};
  // Thread that writes the pipeline cache to disk (WriterCacheMain). It is persistent, because on
  // Horizon detach() closes the game, and wake-ups are decided with the lock held.
  std::thread writer_cache_;
  std::mutex writer_mutex_;
  std::condition_variable writer_warning_;
  bool writer_pending_ = false;
  bool writer_stop_ = false;
  // SaveCachePipelines: with writer_mutex_. The writer takes the cache from the driver; in a match it
  // writes only if the cache grew (otherwise writer_list_waiting_ asks the ring for a save outside the match).
  bool writer_want_cache_ = false;
  bool writer_in_match_ = false;
  bool writer_flush_ = false;  // write the list held back in a match, though nothing is new
  int writer_resave_ = 0;  // bo2zm_native_test_pipelines_resave_s: 1 at once, 2 in slices
  bool writer_verified_ = false;  // the list it writes is all in the cache (ListVerified)
  bool writer_vouch_ = false;     // RequestVouch
  std::atomic<bool> writer_list_waiting_{false};
  std::vector<uint8_t> writer_export_;  // bo2zm_native_test_export_list
  size_t exported_size_ = 0;
  std::chrono::steady_clock::time_point exported_at_{};
  std::atomic<bool> writer_hurry_{false};  // the renderer is stopping: no pauses between slices
  uint32_t pipelines_seen_ = 0;  // ring: pipelines_no_save_ at pipeline_new_at_
  std::chrono::steady_clock::time_point pipeline_new_at_{};
  std::chrono::steady_clock::time_point resave_at_{};
  uint32_t resaves_ = 0;
  // The single pipelines file. The last written version of each part (or the one read at start-up): owned
  // by the writer thread once it is running, and by Init before that. list_read_ goes from
  // LoadCachePipelines to LoadListPipelines.
  std::vector<uint8_t> written_cache_;
  std::vector<uint8_t> written_list_;
  std::vector<uint8_t> list_read_;
  bool files_old_ = false;  // the two older files were read: deleted when the new one is written
  uint64_t ns_pipelines_ = 0;  // creating pipelines (C6 report)
  std::unordered_set<uint32_t> warned_;
  // Pipeline prewarm (LoopPrewarm). list_file_ and state_list_ are sized before the thread
  // starts and never change size; the thread writes state_list_[i] before publishing
  // prewarm_until_ > i. Anything not atomic and not marked otherwise belongs to the ring only.
  std::vector<PipelineRecord> list_file_;         // the list read at start-up (walked by the thread)
  std::vector<uint8_t> state_list_;                   // kList* of each record of list_file_
  std::unordered_map<uint64_t, size_t> index_list_;   // XXH3 of the key -> index (SIZE_MAX: from this run)
  std::vector<PipelineRecord> list_session_;          // this run's new ones
  // bo2pso text lists (LoadPsoList, PsoPipelineCreated): ring only.
  std::unordered_set<uint64_t> pso_listed_;    // keys (XXH3 of PipelineKey) in the loaded list
  std::unordered_set<uint64_t> pso_unlisted_;  // bo2zm_pso_verify: already reported
  std::unordered_set<uint64_t> pso_recorded_;  // bo2zm_pso_record: already written
  std::ofstream pso_record_;
  bool pso_record_checked_ = false;
  bool pso_verify_ = false;
  uint32_t list_no_save_ = 0;
  std::vector<uint8_t> writer_list_;                 // with writer_mutex_
  std::thread prewarm_thread_;
  std::thread prewarm_helper_;  // bo2zm_native_prewarm_threads
  std::atomic<uint32_t> prewarm_compiled_helper_{0};  // set when the helper ends
  std::atomic<bool> prewarm_helper_done_{false};  // the helper has ended (the stamp file waits for it)
  const NativeShaders* library_prewarm_ = nullptr;
  uint32_t prewarm_eds_ = 0;
  bool prewarm_decided_ = false;
  bool prewarm_mismatch_ = false;
  std::chrono::steady_clock::time_point prewarm_start_{};
  std::chrono::steady_clock::time_point prewarm_report_{};
  uint64_t prewarm_changes_previous_ = 0;
  std::atomic<bool> prewarm_stop_{false};
  std::atomic<bool> prewarm_finished_{false};
  std::atomic<size_t> prewarm_until_{0};
  std::atomic<uint32_t> prewarm_done_{0};
  std::atomic<uint32_t> prewarm_compiled_{0};
  std::atomic<uint64_t> prewarm_ns_compiled_{0};
  std::atomic<uint32_t> prewarm_no_shader_{0};
  std::atomic<uint32_t> prewarm_other_mode_{0};
  std::atomic<uint32_t> prewarm_failed_{0};
  std::atomic<int32_t> prewarm_priority_{-1};
  uint32_t prewarm_counted_ = 0;  // of prewarm_compiled_, already added to pipelines_no_save_
  bool precompiled_ = false;      // the precompile at start walked the whole list (Precompile)
  bool vouch_requested_ = false;  // RequestVouch, once
  uint64_t ring_prewarmed_ = 0, ring_prewarmed_slow_ = 0, ns_ring_prewarmed_ = 0;
  uint64_t list_ring_ = 0, list_ns_ring_ = 0;
  uint64_t ring_new_ = 0, ns_ring_new_ = 0, ring_a_list_ = 0;
  // Pipelines compiled on a thread (CompilePipelinesLoop). compile_pending_ is the ring's; the queues are under
  // compile_mutex_.
  int32_t async_compiles_ = -1;  // -1 = not decided yet
  struct PendingCompile {
    uint32_t skipped = 0;  // draws skipped while it compiles
    bool first = false;    // bo2zm_native_compile_first
  };
  std::unordered_map<uint64_t, PendingCompile> compile_pending_;
  uint32_t compile_first_ = 0;  // bo2zm_native_compile_first, read when the renderer starts
  uint64_t compiles_promoted_ = 0, compiles_first_ = 0;
  uint32_t compile_ids_ = 0;  // bo2zm_native_test_compile_log
  std::unordered_set<VkShaderModule> compile_modules_seen_;  // bo2zm_native_test_compile_delay_ms, under compile_mutex_
  std::mutex compile_mutex_;
  std::condition_variable compile_cv_;
  std::deque<CompileJob> compile_queue_;
  std::deque<CompileJob> compile_done_;
  std::atomic<bool> compiles_ready_{false};
  bool compile_stop_ = false;
  std::thread compile_thread_;
  std::thread compile_helper_;  // bo2zm_native_compile_threads
  std::atomic<uint32_t> compiles_helper_{0};
  uint32_t compiles_helper_reported_ = 0;
  uint64_t compiles_async_ = 0, ns_compiles_async_ = 0, compile_ns_worst_ = 0, draws_waiting_pipeline_ = 0;
  uint64_t compiles_reported_ = 0, ns_compiles_reported_ = 0, draws_waiting_reported_ = 0;
  std::chrono::steady_clock::time_point compile_report_{};
};

}  // namespace

std::unique_ptr<VulkanDraws> VulkanDraws::Create(const VulkanDevice* vulkan_device,
                                                    rex::memory::Memory* guest_memory,
                                                    TargetContext* context) {
  if (!vulkan_device || !guest_memory || !context) {
    return nullptr;
  }
  auto draws = std::make_unique<VulkanDrawsImpl>(vulkan_device, guest_memory, context);
  if (!draws->Init()) {
    REXLOG_ERROR("[native] C6: could not prepare the native draws");
    return nullptr;
  }
  REXLOG_INFO("[native] C6: native draws prepared");
  return draws;
}

}  // namespace bo2zm::native
