/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_
#define XENIA_GPU_METAL_METAL_SHARED_MEMORY_H_

// Metal shared memory keeps a Metal-owned guest-memory buffer and updates dirty
// guest ranges through submission-owned upload staging buffers.

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "rex/graphics/metal/metal_upload_buffer_pool.h"
#include "rex/graphics/shared_memory.h"
#include "rex/ui/metal/metal_api.h"

namespace rex {
namespace graphics {
namespace metal {

class MetalCommandProcessor;

// [upload-precommit] per-frame trace counters, written as tr_n_* frame-CSV
// rows by FABLE2_GPU_TIME_TRACE=1 at each guest swap (GPU Commands thread).
struct UploadTraceCounters {
  // Invalid pages uploaded, by last reader (UploadRouteInfo categories).
  uint32_t pg_current = 0, pg_inflight = 0, pg_completed = 0,
           pg_standalone = 0;
  // Render passes ended by a staged upload, by the deciding category: a page
  // the current command buffer read (WAR), a standalone command buffer in
  // flight, only committed command buffers (avoidable), or the same but the
  // pre-commit route was not allowed at that point.
  uint32_t brk_current = 0, brk_standalone = 0, brk_inflight = 0,
           brk_guard = 0;
  // Pre-commit command buffers and pages sent through them.
  uint32_t precommit_cbs = 0, precommit_pages = 0;
  // [texture-precommit] texture-load flushes and loads (tr_n_tex_*).
  // brk: flushes that ended an open render pass; brk_elig: those where every
  // load could go to the pre-commit command buffer. Loads in breaking flushes
  // by source (CPU guest copy / resident shared memory / scaled resolve),
  // first load of the texture, texture sampled by the open command buffer,
  // resident source pages touched by the open (or a standalone) command
  // buffer, guest KB, size (<=16 KB / >256 KB) and eligibility; nopass: loads
  // in flushes without an open render pass; pre: loads sent to pre-commit.
  uint32_t tex_brk = 0, tex_brk_elig = 0, tex_ld = 0, tex_ld_cpu = 0,
           tex_ld_res = 0, tex_ld_scaled = 0, tex_ld_first = 0,
           tex_ld_samp_cur = 0, tex_ld_res_cur = 0, tex_ld_kb = 0,
           tex_ld_small = 0, tex_ld_big = 0, tex_ld_elig = 0,
           tex_ld_nopass = 0, tex_pre_flush = 0, tex_pre_ld = 0,
           tex_pre_fallback = 0, tex_ld_exec = 0, tex_brk_blk_samp = 0,
           tex_brk_blk_res = 0, tex_brk_blk_other = 0, tex_ld_res_gpuw = 0,
           tex_ld_res_copy = 0, tex_ld_res_sa = 0, tex_ld_res_pend = 0,
           tex_ld_res_inv = 0;
  // [tex-rename] sampled_current breaks whose other loads were all eligible.
  uint32_t tex_brk_samp_only = 0;
  // [tex-rename] (tr_n_trn_*): backings renamed, of those from the pool;
  // candidates (sampled by the open command buffer, pass open) left to the
  // split by reason: partial load, RTT keep possible, a queued draw samples
  // the old backing, another load blocker, memory cap, kind (storage,
  // presented, 3D-as-2D); second loads sharing a pending rename; settle
  // copies after a load that did not write the new backing; orphans settled;
  // renamed loads in flushes that still ended the pass; pool / in-flight KB
  // (gauges at the frame end).
  uint32_t trn_rename = 0, trn_pool_hit = 0, trn_fb_partial = 0,
           trn_fb_keep = 0, trn_fb_queued = 0, trn_fb_queued_load = 0, trn_fb_nopass = 0, trn_q_only = 0, trn_noop_stale = 0, trn_noop_keep = 0, nx_same = 0, nx_same_kb = 0, nx_same_renamed = 0, nx_changed = 0, trn_fb_other = 0,
           trn_fb_mem = 0, trn_fb_kind = 0, trn_share = 0, trn_settle = 0,
           trn_orphan = 0, trn_wasted = 0, trn_pool_kb = 0,
           trn_retired_kb = 0;
  // [pass-merge] pre-commit texture flushes that needed a shared-memory
  // upload: kept on the pre-commit route via CPU direct writes; fallbacks
  // with pages read by the open command buffer / in flight / standalone.
  uint32_t tex_pre_direct = 0, tex_fb_cur = 0, tex_fb_inflight = 0,
           tex_fb_sa = 0, tex_pre_staged = 0, dc_dst_merged = 0;
  // [res-scale] scaled-resolve loads in breaking flushes: eligible, blocked
  // by a scaled write (resolve / buffer grow) in the open command buffer, by
  // a scaled range that is not resident yet (the load would grow the buffer),
  // or by invalid shared-memory pages (the load would upload them first).
  uint32_t tex_ld_scl_elig = 0, tex_ld_scl_open = 0, tex_ld_scl_nores = 0,
           tex_ld_scl_inv = 0;
  // [res-scale] copy resolves (tr_n_rsv_*): all, at scale > 1, scaled-resolve
  // buffer grows (old ranges copied); all scaled-resolve texture loads.
  uint32_t rsv_copy = 0, rsv_scaled = 0, rsv_grow = 0, tex_ld_scl_all = 0;
  // [edram-transfer] ownership transfers (tr_n_xfer_*): transfers, destination
  // render targets with transfers, and the transfer kind by source/destination
  // key: same format and MSAA (base or pitch change), other colour format,
  // colour <-> depth, MSAA change, with a host depth source. q / fb:
  // destinations queued into the draw pass / sent to a separate transfer pass.
  uint32_t xfer_n = 0, xfer_rt = 0, xfer_same = 0, xfer_fmt = 0, xfer_cd = 0,
           xfer_msaa = 0, xfer_hostdepth = 0, xfer_q = 0, xfer_fb = 0;
  // [rt-store-audit] draws with stencil test / stencil write enabled, draws
  // with a depth-stencil target bound.
  uint32_t draw_stencil = 0, draw_ds = 0;
  // [effects-native] lever-5 classification (FABLE2_EFFECTS_NATIVE_TRACE=1,
  // diagnostic only, no behaviour change). An eligible target is a colour,
  // 1xMSAA target at most half as wide as the widest depth-tested scene pass.
  // draw: draws whose bound targets are all eligible and use no depth/stencil;
  // draw_depth: draws binding an eligible colour target together with
  // depth/stencil (that target could not go native in that pass);
  // xfer_native / xfer_mixed: transfers between two eligible targets / between
  // an eligible and a full-scale target (would need mixed-scale transfers);
  // rsv / rsv_mixed: copy resolves whose source tiles are all / partly owned by
  // eligible targets.
  uint32_t eff_draw = 0, eff_draw_depth = 0, eff_xfer_native = 0,
           eff_xfer_mixed = 0, eff_rsv = 0, eff_rsv_mixed = 0;
  // [native-scale] transfers encoded between different scales, resolves
  // done at native scale, mixed resolves unified by ownership transfers,
  // draws into native-scale shadow-map depth targets, host-depth sources
  // dropped from mixed-scale depth transfers.
  uint32_t eff_xfer_scaled = 0, eff_rsv_native = 0, eff_rsv_unified = 0,
           sh_draw = 0, sh_hostdepth_drop = 0, ts_draw = 0;
  // [cb-end] main command buffers ended, by cause (tr_n_cbend_*): PM4 primary buffer
  // end, worker idle (PrepareForWait), shared-memory read dependency, explicit wait.
  uint32_t cbend_pbe = 0, cbend_idle = 0, cbend_dep = 0, cbend_wait = 0;
  // [tile-edram] separate transfer/clear render passes by kind (tr_n_xfp_*):
  // resolve clear only, transfers with a self-source host depth (need
  // rt_depth_store), other transfers; estimated attachment KB loaded + stored
  // by them (touched rectangles x samples x bytes per sample; a Load counts
  // the rectangles, a Store counts them again); K1 self-depth destinations
  // moved into the draw pass (tr_n_tile_selfdepth).
  uint32_t xfp_clear = 0, xfp_self = 0, xfp_other = 0, xfp_kb = 0,
           xfp_clear_kb = 0, tile_selfdepth = 0;
  // [deferred-clears] resolve clears deferred (tr_n_dc_*): deferred, drawn
  // first inside the next draw pass that binds the target (merged), merged
  // clears covering the whole target (would have allowed loadAction=Clear),
  // flushed early as a separate pass by reason, touched-rect KB of separate
  // clear-pass load+store avoided by the merges.
  uint32_t dc_deferred = 0, dc_merged = 0, dc_fullrect = 0, dc_fl_resolve = 0,
           dc_fl_xfer = 0, dc_fl_reclear = 0, dc_fl_preflight = 0,
           dc_fl_guard = 0, dc_fl_swap = 0, dc_fl_other = 0, dc_saved_kb = 0,
           dc_requeue = 0;
  // [alias-clear] (tr_n_ac_*): XDK alias clear quads done as EDRAM clears
  // (draws, surfaces), of those queued as deferred clears or drawn at once
  // (partly covered tiles of other owners); no-op draws skipped (nothing
  // bound, written or exported); candidates left as draws by reason: state
  // (blend, mask, shader, scissor...), value (not exactly representable),
  // owner (no other MSAA variant), rect (origin, alignment, pitch).
  uint32_t ac_clear = 0, ac_surfaces = 0, ac_deferred = 0, ac_immediate = 0,
           ac_noop = 0, ac_fb_state = 0, ac_fb_value = 0, ac_fb_owner = 0,
           ac_fb_rect = 0, ac_cross_format = 0;
  // [main-depth] (tr_n_md_*): depth words written into a unorm colour
  // variant, stencil-only alias clears, and the transfers those moved into the
  // same-format variant.
  // md_carried: a queued depth-word clear of the variant drawn in the
  // stencil-only clear's transfer pass instead of a pass of its own.
  uint32_t md_word = 0, md_stencil = 0, md_stencil_xfer = 0, md_carried = 0;
  // [ssmask] (tr_n_ssm_clear_la, docs/ssmask-20261014.md): in-pass
  // whole-target deferred clears turned into the pass's clear load action.
  uint32_t ssm_clear_la = 0;
  // [ssm-merge] FABLE2_SSM_MERGE (docs/ssm-merge-20261015.md): stencil-only
  // alias clears whose transfers and clears went into the next draw pass
  // (merged), went the standalone way at the next cache call (flushed), and
  // passes whose depth load became DontCare because the in-pass clear and
  // transfers cover the whole target (dontcare).
  uint32_t ssm_merged = 0, ssm_flushed = 0, ssm_dontcare = 0;
  // ssm_folded: dead stencil-only clears dropped (FABLE2_SSM_STENCIL_FOLD).
  uint32_t ssm_folded = 0;
  // clr_const: transfer sources read from a constant clear texture
  // (FABLE2_CLEAR_CONST_SOURCE) instead of flushing their pending clear.
  uint32_t clr_const = 0;
  // [ssmask] FABLE2_EFFECTS_FOOTPRINT_VARIANT: draws given the pixel-shader
  // variant with the footprint code, and draws whose variant without it met
  // a scaled texture behind a non-depth offset fetch (must stay 0).
  uint32_t fp_variant = 0, fp_variant_miss = 0;
  // [raster-state] draw-pass transfer batches (tr_n_xq_*): encoded into a draw
  // pass, of those after a guest draw set rasterizer state in the same
  // encoder, and of those with an inherited non-default cull mode, fill mode,
  // depth bias or depth clip mode (measured before the reset).
  uint32_t xq_inpass = 0, xq_reused = 0, xq_inh_cull = 0, xq_inh_fill = 0,
           xq_inh_bias = 0, xq_inh_clip = 0;
  // [tex-blit] texture upload buffer->texture copies (tr_n_tex_blit_*): KB
  // copied, KB and count into resolution-scaled resolve textures.
  uint32_t tex_blit_kb = 0, tex_blit_scl_kb = 0, tex_blit_scl = 0;
  // [tex-load-direct] scaled loads written straight into the texture and the
  // KB they wrote (the staging-buffer write + blit they no longer need).
  uint32_t tex_dl = 0, tex_dl_kb = 0;
  // [resolve-to-texture] (tr_n_rtt_*): scaled copy resolves, of those with a
  // cached scaled texture on the destination base page; textures classified
  // at resolve time (RttClass 1..5); scaled base-level loads and their scaled
  // KB by the class of the last resolve that wrote them (0 = none seen);
  // textures written straight by a resolve (or, FABLE2_RTT_DIAG=2, skipped).
  uint32_t rtt_rsv = 0, rtt_rsv_hit = 0;
  uint32_t rtt_t[6] = {};
  uint32_t rtt_ld[6] = {}, rtt_ld_kb[6] = {};
  uint32_t rtt_direct = 0, rtt_direct_kb = 0;
  // loads skipped because the texture still holds the buffer's content
  // (memexport-only invalidation), and their scaled KB.
  uint32_t rtt_keep = 0, rtt_keep_kb = 0;
  // fused writes invalidated before any sample (KB; includes the first half
  // of a texture resolved in two parts).
  uint32_t rtt_waste_kb = 0;
  // [tex-resolve] fused writes into a whole mip level, and mips loads
  // skipped because all stored mip levels were still valid.
  uint32_t rtt_mip_direct = 0, rtt_mip_keep = 0;
  // [rtt-1x] global-watch invalidations seen by resolve-to-texture, the
  // tracked textures scanned by them, and the tracked texture count.
  uint32_t rtt_inv = 0, rtt_scan = 0, rtt_trk = 0;
  // [rt-shrink] render-target growths and the KB copied by them.
  uint32_t rts_grow = 0, rts_grow_kb = 0;
  // [tex-resolve] lazy-buffer census (FABLE2_LAZY_CENSUS=1), scaled KB:
  // candidate writes, overwritten unread, read by loads / morph readback,
  // all resolve writes.
  uint32_t lz_cand_kb = 0, lz_dead_kb = 0, lz_read_kb = 0, lz_morph_kb = 0,
           lz_all_kb = 0;
  // [tile-resolve] (tr_n_tres_*): colour copies done as a tile function in
  // the draw pass; draw passes offered (kept open); copies left on the compute
  // path by reason: precheck (growth, deferred clears, rt-shrink verify), shape
  // (MSAA, depth, uint view, sources, half-pixel rows), pass (source not colour
  // attachment 0 of the open pass, or the pass ended during preparation), area
  // (render area > ratio x resolve rectangle), hazard (a fused target sampled
  // in the pass, memexport into the destination), pso (tile pipeline not
  // built yet or failed), tile (Metal tile width not a multiple of the
  // pixels per thread).
  uint32_t tres_tile = 0, tres_offer = 0, tres_fb_precheck = 0,
           tres_fb_shape = 0, tres_fb_pass = 0, tres_fb_area = 0,
           tres_fb_hazard = 0, tres_fb_pso = 0, tres_fb_tile = 0;
  // [tile-resolve extent] draw passes opened with a render area shorter than
  // their targets (shrunk), passes ended because a draw needed more rows
  // (grow), and tile resolves whose source rows lie outside a shrunk area
  // (fb_extent, compute path).
  uint32_t tres_shrunk = 0, tres_grow = 0, tres_fb_extent = 0;
  // [tile-resolve keep] resolve clears drawn in the tile resolve's pass,
  // tile resolves whose pass stayed open after the resolve (KEEP_OPEN), and
  // kept passes ended because a draw read what the tile resolve wrote.
  uint32_t tres_clear_inpass = 0, tres_kept = 0, tres_kept_break = 0;
  // [mip-lod] tile resolves into a stored mip level admitted because every
  // draw of the pass that samples the texture is bounded below that level;
  // draws sampling a candidate texture that were bounded (with a view
  // limited to fewer levels than the texture has: view) or not.
  uint32_t tres_miplod = 0, miplod_bounded = 0, miplod_view = 0,
           miplod_unbounded = 0;
};
UploadTraceCounters& UploadTrace();

class MetalSharedMemory : public SharedMemory {
 public:
  MetalSharedMemory(MetalCommandProcessor& command_processor, rex::memory::Memory& memory);
  ~MetalSharedMemory() override;
  bool Initialize();
  void Shutdown();
  void ClearCache() override;

  MTL::Buffer* GetBuffer() const { return buffer_; }
  const uint8_t* GetXboxRamBase() const {
    return static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  }
  // Diagnostic: write-watch state of a physical range in every guest view.
  std::string DescribeWatchState(uint32_t start, uint32_t length) const {
    return memory().DescribePhysicalWatchState(start, length);
  }
  void MarkGpuAccess(uint32_t start, uint32_t length,
                     uint64_t submission_index);
  void TrackStandaloneGpuAccess(MTL::CommandBuffer* command_buffer,
                                const std::pair<uint32_t, uint32_t>* ranges,
                                uint32_t range_count);
  struct UploadRouteInfo {
    uint64_t upload_bytes = 0;
    uint64_t direct_bytes = 0;
    uint64_t staged_bytes = 0;
    // [upload-precommit] staged bytes that must be encoded in the current
    // command buffer (so they end an active render pass), and the subset of
    // staged_bytes that can go to a pre-commit command buffer instead.
    uint64_t inpass_staged_bytes = 0;
    uint64_t precommit_bytes = 0;
    // Invalid pages by who last read them: the open command buffer, a
    // committed one still in flight, a completed one, or a standalone
    // command buffer still in flight.
    uint32_t pages_current = 0;
    uint32_t pages_inflight = 0;
    uint32_t pages_completed = 0;
    uint32_t pages_standalone = 0;
  };
  // [texture-precommit] true when any page of the range was last accessed by
  // the open main command buffer or is in use by a standalone one.
  bool RangeTouchedByOpenOrStandalone(uint32_t start, uint32_t length);
  // [texture-precommit] GPU writes into the shared buffer (resolves,
  // memexport, in-command-buffer staged copies) by submission, and whether
  // the open main command buffer wrote any page of a range or a standalone
  // command buffer still uses one. Reads do not count here.
  void MarkGpuWrite(uint32_t start, uint32_t length, uint64_t submission_index,
                    bool staged_copy = false);
  // Trace-only diagnosis: pre-commit staged copies by submission (bit 32 of
  // RangeOpenWriteReasons; never used for routing).
  void MarkPrecommitCopy(uint32_t start, uint32_t length,
                         uint64_t submission_index);
  // [mip-lod] the GPU-visible bytes of a valid range that no GPU write,
  // staged copy or standalone command buffer still in flight can change
  // (shared storage); null otherwise.
  const uint8_t* StableContents(uint32_t start, uint32_t length);
  bool RangeWrittenByOpenOrStandalone(uint32_t start, uint32_t length) {
    return (RangeOpenWriteReasons(start, length) & 7u) != 0;
  }
  // Bits: 1 GPU write (resolve / memexport), 2 staged copy, 4 standalone,
  // 32 pre-commit copy for the open command buffer (diagnostic only).
  uint32_t RangeOpenWriteReasons(uint32_t start, uint32_t length);
  UploadRouteInfo GetUploadRouteInfo(const SharedMemory::Range* ranges,
                                     uint32_t range_count);

  // [upload-precommit] who last read a page (see UploadRouteInfo).
  enum class PageAccess : uint8_t {
    kCompleted,
    kInflight,
    kCurrent,
    kStandalone,
  };
  // FABLE2_UPLOAD_PRECOMMIT (default on; =0 kill switch): staged uploads whose
  // pages were last read only by committed command buffers go to a small
  // command buffer committed ahead of the current one instead of ending the
  // active render pass. docs/weak-mac-perf-20261008.md.
  static bool UploadPrecommitEnabled();

  // For trace dump, simplified - just make buffer available for reading
  void UseForReading() {
    // No state transitions needed in Metal
  }
  // Override pure virtual function from SharedMemory
  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_upload_ranges) override;

  // FABLE2_MEMEXPORT_PRESERVE (default on; =0 disables): remember memexport-written
  // ranges with a hash of their guest bytes at export time, so a later
  // re-upload of an invalidated page does not overwrite GPU-written data with
  // stale guest RAM unless the CPU really changed those bytes.
  static bool MemexportPreserveEnabled();
  void NoteMemexportWrite(uint32_t start, uint32_t length);

  // FABLE2_MEMEXPORT_READBACK (default on; =0 disables): Metal counterpart of the SDK's
  // readback_memexport. Each memexport range is queued with the submission
  // that writes it and a hash of its guest bytes at export time. Once that
  // submission has completed, the GPU-written bytes are copied from the
  // (shared-storage) buffer back into guest RAM through the unwatched
  // physical view, unless the CPU changed those guest bytes in the meantime.
  // No GPU waits: applied from ProcessCompletedSubmissions and before any
  // re-upload in UploadRanges.
  static bool MemexportReadbackEnabled();
  // FABLE2_SHMEM_VALID_FIRST (default on; CR-02): the direct-write upload
  // route marks a range valid before copying it, as the staged route does.
  static bool ShmemValidFirstEnabled();
  void NoteMemexportReadback(uint32_t start, uint32_t length,
                             uint64_t submission);
  // [memexport-shadow] REX_METAL_MEMEXPORT_CPU=4 (diagnostic): the GPU still
  // does the block copy; the bytes the copy should produce are worked out on
  // the CPU from guest memory at draw time and compared with the GPU buffer
  // once the draw's submission has completed.
  struct MemexportShadow {
    uint32_t start = 0;  // destination span
    uint32_t end = 0;
    uint32_t src_start = 0;  // source range read through the vertex fetch
    uint32_t src_end = 0;
    uint64_t submission = 0;
    uint64_t seq_first = 0;  // the draw's own readback entries
    uint64_t seq_last = 0;
    bool src_pending = false;  // source overlapped a pending export
    bool src_invalid = false;  // source was not valid in the buffer
    uint32_t src_div_words = 0;  // settled source words: buffer != guest
    // Source at draw time (before the draw's own source request).
    std::vector<uint8_t> src_snap;  // guest bytes the model read
    uint32_t src_pages = 0;
    uint32_t src_pages_invalid = 0;
    uint32_t src_pages_gpu_write = 0;  // GPU write not completed
    uint32_t src_pages_staged = 0;  // staged upload not completed
    uint32_t src_pages_precommit = 0;  // precommit upload not completed
    uint32_t src_pages_standalone = 0;
    uint32_t src_readbacks_pending = 0;
    // Destination words written again with a different value by a later
    // vertex of the same draw: (byte offset, value that lost).
    std::vector<std::pair<uint32_t, uint32_t>> losers;
    uint32_t rewritten_words = 0;  // written more than once, any value
    // Destination at model time (before the draw).
    std::vector<uint8_t> dst_buf_before;  // GPU buffer bytes of the span
    std::vector<uint8_t> dst_page_invalid;
    uint32_t dst_pages = 0;
    uint32_t dst_pages_invalid = 0;
    uint32_t dst_pages_gpu_write = 0;
    uint32_t dst_pages_staged = 0;
    uint32_t dst_pages_precommit = 0;
    uint32_t dst_readbacks_pending = 0;
    // Runs the copy model against another source image, into a copy of
    // `before`.
    std::function<void(const uint8_t*, std::vector<uint8_t>&)> rerun;
    std::vector<uint8_t> expected;
    std::vector<uint8_t> before;
    std::vector<uint8_t> written;  // 1 per byte the copy writes
  };
  void NoteMemexportShadow(MemexportShadow&& shadow, uint64_t submission,
                           uint32_t own_ranges);
  void NoteMemexportSourceState(MemexportShadow& shadow);
  void NoteMemexportDestinationState(MemexportShadow& shadow);
  // [shmem-diverge] Compares the GPU buffer with guest memory over a copy's
  // source range, only on pages that are valid and have no GPU write, staged
  // copy or export readback outstanding. Such a page must be identical on
  // both sides; returns the number of words that are not.
  uint32_t CheckMemexportSourceDivergence(uint32_t start, uint32_t length,
                                          uint32_t destination);
  // REX_METAL_MEMEXPORT_GUARD (default on; =0 disables). A memexport range is
  // uploaded before its draw and marked valid again after it. A CPU write in
  // between (a streamed image landing in memory the range still names)
  // invalidates its pages; marking them valid again would leave the buffer
  // with the bytes from before that write, and the readback would then copy
  // those old bytes over the new guest data. With the guard such pages stay
  // invalid, and the readback leaves out every page that is invalid when it
  // runs: the CPU wrote there after the export, so its data wins.
  static bool MemexportGuardEnabled();
  // REX_METAL_MEMEXPORT_MERGE (default on; =0 keeps the old behaviour and only
  // counts). A streamed BO2 block can hold two images interleaved inside the
  // same pages: one written by the CPU loader, the other by a GPU block move
  // (memexport). The export declares the whole stream range but writes only
  // its own image's bytes. When the CPU changes any byte of the range between
  // the export draw and its readback, the old code dropped the whole export.
  // With the merge, the readback copies back exactly the words the export
  // changed and the CPU did not.
  static bool MemexportMergeEnabled();
  // RangeWrittenByGpu for a memexport range, with the guard above.
  void MemexportRangeWritten(uint32_t start, uint32_t length);
  void ApplyCompletedMemexportReadbacks(uint64_t completed_submission);
  // True while a memexport draw's data has not been copied back to guest
  // memory yet (its submission has not completed, or completed unprocessed).
  bool HasPendingMemexportReadbacks() const {
    return !pending_readbacks_.empty();
  }
  // [upload-precommit] trace-only check (FABLE2_UPLOAD_VERIFY=1): once the
  // submission of a staged copy (in-pass or pre-commit) has completed, the
  // GPU buffer must hold the copied bytes wherever guest RAM still holds
  // them. Counts per route, logged as [upload-verify].
  static bool UploadVerifyEnabled();
  void VerifyCompletedUploads(uint64_t completed_submission);

 private:
  struct PendingReadback {
    uint32_t start;
    uint32_t end;
    uint64_t submission;
    uint64_t guest_hash;
    uint64_t export_frame;
    // Guest bytes of the range when the export was drawn (empty for a range
    // above the snapshot limit). Tells apart what the export wrote from what
    // the CPU wrote afterwards.
    std::vector<uint8_t> before;
    // [memexport-hole] wall clock of the export draw, and whether an upload
    // of guest memory went over this range while it was still pending.
    uint64_t export_us = 0;
    bool hole = false;
    uint64_t seq = 0;
  };
  std::deque<PendingReadback> pending_readbacks_;
  // [memexport-shadow]
  void CheckCompletedMemexportShadows(uint64_t completed_submission);
  std::deque<MemexportShadow> memexport_shadows_;
  uint64_t readback_seq_ = 0;
  uint64_t shadow_draws_ = 0;
  uint64_t shadow_draws_bad_ = 0;       // any missing or wrong word
  uint64_t shadow_words_ok_ = 0;
  uint64_t shadow_words_later_export_ = 0;  // a later export overlaps
  uint64_t shadow_words_cpu_later_ = 0;     // buffer holds a later CPU write
  uint64_t shadow_words_missing_ = 0;       // buffer holds pre-copy bytes
  uint64_t shadow_words_wrong_ = 0;         // buffer holds something else
  uint64_t shadow_words_wrong_src_ = 0;     // ... source pending or invalid
  uint64_t shadow_draws_src_pending_ = 0;
  uint64_t shadow_draws_src_invalid_ = 0;
  uint64_t shadow_draws_hole_ = 0;
  uint64_t shadow_dropped_ = 0;
  uint32_t shadow_logged_ = 0;
  uint64_t shadow_draws_bad_nopend_ = 0;       // bad, source not pending
  uint64_t shadow_draws_bad_div_ = 0;          // bad, source diverged
  uint64_t shadow_draws_bad_unexplained_ = 0;  // bad, neither
  uint64_t shadow_words_bad_unexplained_ = 0;
  // Bad copies whose source was not pending, by what the source did.
  uint64_t shadow_bad_src_guest_changed_ = 0;
  uint64_t shadow_bad_src_buffer_only_ = 0;
  uint64_t shadow_bad_src_same_ = 0;
  uint64_t shadow_bad_src_unknown_ = 0;
  uint64_t shadow_bad_words_as_buffer_ = 0;
  uint64_t shadow_bad_words_as_guest_ = 0;
  uint64_t shadow_bad_words_as_neither_ = 0;
  uint64_t shadow_bad_src_not_settled_ = 0;
  uint32_t shadow_src_logged_ = 0;
  uint64_t shadow_dup_copies_ = 0;  // copies with a contested word
  uint64_t shadow_dup_rewritten_words_ = 0;
  uint64_t shadow_dup_contested_words_ = 0;
  uint64_t shadow_dup_bad_copies_ = 0;  // bad copies with a loser match
  uint64_t shadow_dup_bad_words_ = 0;  // bad words equal to a loser
  uint64_t shadow_dup_bad_words_not_pending_ = 0;
  uint64_t shadow_bad_words_all_ = 0;
  uint64_t shadow_bad_words_not_pending_ = 0;
  uint32_t shadow_dup_logged_ = 0;
  uint32_t shadow_dst_logged_ = 0;
  // Checked copies kept for a while: who else wrote the same words.
  std::deque<MemexportShadow> shadow_history_;
  uint64_t shadow_history_bytes_ = 0;
  uint32_t shadow_prev_logged_ = 0;
  uint64_t shadow_prev_copies_ = 0;        // share a written word with an earlier copy
  uint64_t shadow_prev_copies_same_sub_ = 0;
  uint64_t shadow_prev_copies_bad_ = 0;
  uint64_t shadow_prev_shared_words_ = 0;
  uint64_t shadow_prev_shared_diff_words_ = 0;  // the two copies write different values
  uint64_t shadow_prev_bad_words_ = 0;     // bad words (all copies)
  uint64_t shadow_prev_bad_as_earlier_ = 0;
  uint64_t shadow_prev_bad_earlier_other_ = 0;
  uint64_t shadow_prev_bad_no_earlier_ = 0;
  uint64_t shadow_prev_np_bad_words_ = 0;  // source not pending only
  uint64_t shadow_prev_np_bad_as_earlier_ = 0;
  uint64_t shadow_prev_np_bad_earlier_other_ = 0;
  uint64_t shadow_prev_np_bad_no_earlier_ = 0;
  uint64_t shadow_dst_copies_ = 0;
  uint64_t shadow_dst_bad_words_ = 0;
  uint64_t shadow_dst_as_buf_before_ = 0;
  uint64_t shadow_dst_on_invalid_page_ = 0;
  uint64_t shadow_dst_runs_ = 0;
  uint64_t shadow_dst_runs_page_start_ = 0;
  uint64_t shadow_dst_found_expected_ = 0;
  uint64_t shadow_dst_found_source_ = 0;
  uint64_t shadow_dst_found_nowhere_ = 0;
  uint64_t src_div_checks_ = 0;
  uint64_t src_div_settled_bytes_ = 0;
  uint64_t src_div_pages_ = 0;
  uint64_t src_div_words_ = 0;
  uint64_t src_div_words_guest_zero_ = 0;
  uint64_t src_div_words_buffer_zero_ = 0;
  uint32_t src_div_logged_ = 0;
  // [memexport-hole] uploads of guest memory over a pending export range
  // that no preserve entry protects.
  uint64_t hole_events_ = 0;
  uint64_t hole_bytes_ = 0;
  uint64_t hole_events_guest_same_ = 0;  // guest unchanged since the export
  uint64_t hole_events_expired_ = 0;     // older than the preserve lifetime
  uint64_t hole_kept_bytes_ = 0;         // REX_METAL_MEMEXPORT_PENDING_KEEP=1
  uint64_t hole_readbacks_ = 0;          // readbacks that land after a hole
  uint64_t hole_readbacks_wiped_ = 0;    // ... buffer equals pre-export bytes
  uint32_t hole_logged_ = 0;
  // [memexport-merge] counters.
  uint64_t merge_ranges_ = 0;        // readbacks whose guest bytes changed
  uint64_t merge_ranges_lost_ = 0;   // ... dropped whole (merge off / no snapshot)
  uint64_t merge_words_cpu_ = 0;     // words only the CPU changed
  uint64_t merge_words_gpu_ = 0;     // words only the export changed
  uint64_t merge_words_both_ = 0;    // words both changed
  uint64_t merge_words_applied_ = 0;
  uint32_t merge_logged_ = 0;
  uint64_t preserve_dropped_ = 0;          // re-upload over a changed export range
  uint64_t preserve_dropped_pending_ = 0;  // ... whose readback is still pending
  uint64_t preserve_dropped_gpu_words_ = 0;
  uint32_t preserve_dropped_logged_ = 0;
  // Memexport guard counters, logged as [memexport-guard].
  uint64_t guard_draw_ranges_ = 0;
  uint64_t guard_draw_ranges_hit_ = 0;
  uint64_t guard_draw_pages_invalid_ = 0;
  uint64_t guard_readback_ranges_hit_ = 0;
  uint64_t guard_readback_pages_invalid_ = 0;
  uint64_t guard_readback_pages_stale_ = 0;
  uint64_t guard_last_summary_ = 0;
  struct ReadbackRegionStats {
    uint64_t ranges = 0;
    uint64_t bytes = 0;
    uint64_t changed_chunks = 0;
  };
  std::map<uint32_t, ReadbackRegionStats> readback_regions_;
  uint32_t readback_region_logged_ = 0;
  struct PendingVerify {
    uint32_t start;
    uint32_t size;
    uint64_t hash;
    uint64_t submission;
    bool precommit;
  };
  std::deque<PendingVerify> pending_verifies_;
  void NoteUploadVerify(uint32_t start, uint32_t size, bool precommit);
  struct PreservedExport {
    uint32_t start;
    uint32_t end;
    uint64_t guest_hash;
    uint64_t last_export_us;
  };
  std::vector<PreservedExport> preserved_exports_;
  void ExpirePreservedExports(uint64_t now_us);
  // Caller holds standalone_gpu_access_mutex_.
  PageAccess ClassifyPageAccessLocked(uint32_t page, uint64_t completed,
                                      uint64_t committed,
                                      uint64_t* last_access) const;
  MetalCommandProcessor& command_processor_;
  std::unique_ptr<MetalUploadBufferPool> upload_buffer_pool_;
  std::vector<uint64_t> page_last_main_gpu_access_submission_;
  // MarkGpuAccess dedupe: draws in one submission mark the same vertex and
  // index ranges again and again (four times per frame in split screen), and
  // each call walks every page of the range. A range already marked for the
  // current submission is skipped; the result is identical because the page
  // values only ever take the max.
  struct MarkedGpuAccessRange {
    uint32_t start = 0;
    uint32_t length = 0;
    uint64_t submission = 0;
  };
  static constexpr uint32_t kMarkedGpuAccessCacheSize = 1024;
  std::array<MarkedGpuAccessRange, kMarkedGpuAccessCacheSize>
      marked_gpu_access_cache_{};
  bool dedupe_gpu_access_ = true;
  std::vector<uint64_t> page_last_gpu_write_submission_;
  std::vector<uint64_t> page_last_staged_copy_submission_;
  std::vector<uint64_t> page_last_precommit_copy_submission_;
  std::mutex standalone_gpu_access_mutex_;
  std::vector<uint32_t> page_standalone_gpu_access_counts_;
  MTL::Buffer* buffer_ = nullptr;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif
