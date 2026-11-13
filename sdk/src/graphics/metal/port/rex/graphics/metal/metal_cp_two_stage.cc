// [cp-two-stage] Two-stage command processor prototype (metal_cp_two_stage,
// REX_METAL_CP_TWO_STAGE=1; default off).
//
// Stage 1 = the GPU command processor thread. It parses PM4 exactly as before,
// keeps the guest-visible register file (CommandProcessor::register_file_),
// writes fences, scratch registers and interrupts, and for every draw it
// snapshots the guest memory the draw will read (vertex fetch, index and
// texture ranges, resolve destinations) into the batch being recorded. It
// never touches Metal.
//
// Stage 2 = a thread of its own. It replays the register-write log into its own
// register file (the one every backend object was created with), uploads the
// snapshotted runs, and runs IssueDraw and everything below it, so the Metal
// encoding of a submission no longer delays the fence the guest waits on.
//
// Barriers (stage 1 queues a closure and waits for stage 2 to run it after
// everything before it): swap, interrupts, sleeping WAIT_REG_MEM, occlusion
// queries (unless REX_METAL_CP_TWO_STAGE_ZPD_ASYNC=1), trace readback, draws
// that read guest memory on the CPU at encode time or write it (memexport,
// tessellation, index conversion / primitive reset), cache maintenance and
// shutdown. While stage 2 runs a barrier closure stage 1 is parked, so the
// closure may use the guest register file and live guest memory.

#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "metal_flags.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_render_target_cache.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/graphics/metal/metal_texture_cache.h"
#include "rex/graphics/registers.h"
#include "rex/graphics/util/draw.h"
#include "rex/graphics/xenos.h"
#include "rex/logging.h"
#include "rex/memory/utils.h"
#include "third_party/metal-cpp/Foundation/NSAutoreleasePool.hpp"

namespace rex {
namespace graphics {
namespace metal {

namespace {

thread_local bool tls_ts_stage2 = false;

uint64_t ThreadCpuNs() { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); }
uint64_t MonoNs() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

bool EnvOn(const char* name, bool fallback) {
  const char* v = MetalDebugEnv(name);  // getenv-ok: once, at TsStart
  if (!v || !v[0]) {
    return fallback;
  }
  return v[0] != '0';
}

constexpr uint32_t kNoAlias = UINT32_MAX;
constexpr uint32_t kDrawIndexed = 1u << 0;
constexpr uint32_t kDrawMajorModeExplicit = 1u << 1;
// kDraw payload words (after the op word).
constexpr uint32_t kDrawWords = 14;

const char* const kSyncNames[] = {
    "swap",        "interrupt",  "zpd",          "wait_reg_mem", "trace_readback", "trace_drain",
    "memexport",   "tessellation", "index_conversion", "maintenance", "shutdown",
};

}  // namespace

bool MetalCommandProcessor::TsOnStage1() const { return !tls_ts_stage2; }

void MetalCommandProcessor::TsStart() {
  ts_zpd_async_ = EnvOn("CP_TWO_STAGE_ZPD_ASYNC", false);
  ts_interrupt_barrier_ = !EnvOn("CP_TWO_STAGE_INTERRUPT_ASYNC", false);
  ts_verify_registers_ = EnvOn("CP_TWO_STAGE_VERIFY", false);
  ts_log_misses_ = EnvOn("CP_TWO_STAGE_LOG_MISSES", false);
  ts_sync_all_draws_ = EnvOn("CP_TWO_STAGE_SYNC_DRAWS", false);
  ts_flush_queue_always_ = EnvOn("CP_TWO_STAGE_FLUSH_QUEUE_ALWAYS", false);
  ts_sync_resolves_ = EnvOn("CP_TWO_STAGE_SYNC_RESOLVES", false);
  if (const char* v = MetalDebugEnv("CP_TWO_STAGE_PUBLISH_DRAWS")) {  // getenv-ok: once
    int n = std::atoi(v);
    if (n > 0) {
      ts_publish_draws_ = uint32_t(n);
    }
  }
  // A command buffer (and its autorelease pool) begun on this thread must end
  // here: from now on only stage 2 touches Metal.
  TsEndFrameWork("cp two-stage start");
  if (!ts_batch_) {
    ts_batch_ = std::make_unique<TsBatch>();
  }
  ts_vertex_shader_ = CommandProcessor::active_vertex_shader();
  ts_pixel_shader_ = CommandProcessor::active_pixel_shader();
  ts_active_ = true;
  if (!ts_thread_.joinable()) {
    ts_stop_ = false;
    ts_thread_ = std::thread([this] { TsThreadMain(); });
  }
  REXLOG_INFO(
      "[cp-two-stage] on: publish every {} draws, zpd {}, interrupts {}, verify {}",
      ts_publish_draws_, ts_zpd_async_ ? "async" : "barrier",
      ts_interrupt_barrier_ ? "barrier" : "async", ts_verify_registers_ ? "on" : "off");
}

void MetalCommandProcessor::TsEndFrameWork(const char* why) {
  if (current_command_buffer_ || command_buffer_autorelease_pool_) {
    FlushCommandBufferAndWait(5000000000ULL, why);
  }
}

void MetalCommandProcessor::TsStop() {
  if (!ts_thread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(ts_mutex_);
    ts_stop_ = true;
  }
  ts_work_cv_.notify_all();
  ts_thread_.join();
  TsLogStats("shutdown");
  ts_active_ = false;
}

void MetalCommandProcessor::TsThreadMain() {
  tls_ts_stage2 = true;
  pthread_setname_np("GPU Commands 2");
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  // Objects autoreleased outside a command buffer's pool land here; drained
  // only when no command-buffer pool sits above it on this thread's stack.
  NS::AutoreleasePool* outer_pool = nullptr;
  for (;;) {
    std::unique_ptr<TsBatch> batch;
    {
      std::unique_lock<std::mutex> lock(ts_mutex_);
      if (ts_queue_.empty() && !ts_stop_) {
        ts_stage2_idle_ = true;
        ts_work_cv_.wait(lock, [this] { return !ts_queue_.empty() || ts_stop_; });
        ts_stage2_idle_ = false;
      }
      if (ts_queue_.empty()) {
        break;
      }
      batch = std::move(ts_queue_.front());
      ts_queue_.pop_front();
    }
    const uint64_t cpu0 = ThreadCpuNs();
    if (!outer_pool && !command_buffer_autorelease_pool_) {
      outer_pool = NS::AutoreleasePool::alloc()->init();
    }
    TsExecuteBatch(*batch);
    if (outer_pool && !command_buffer_autorelease_pool_) {
      outer_pool->release();
      outer_pool = nullptr;
    }
    ts_stats_.stage2_cpu_ns += ThreadCpuNs() - cpu0;
    ++ts_stats_.stage2_batches;
    {
      std::lock_guard<std::mutex> lock(ts_mutex_);
      ts_done_seq_ = batch->seq;
      batch->Clear();
      ts_free_.push_back(std::move(batch));
    }
    ts_done_cv_.notify_all();
  }
  if (outer_pool && !command_buffer_autorelease_pool_) {
    outer_pool->release();
  }
}

void MetalCommandProcessor::TsPublish() {
  if (!ts_batch_ || ts_batch_->ops.empty()) {
    return;
  }
  bool wake = false;
  {
    std::lock_guard<std::mutex> lock(ts_mutex_);
    ts_batch_->seq = ts_next_seq_++;
    ts_queue_.push_back(std::move(ts_batch_));
    wake = ts_stage2_idle_;
    if (!ts_free_.empty()) {
      ts_batch_ = std::move(ts_free_.back());
      ts_free_.pop_back();
    }
  }
  if (!ts_batch_) {
    ts_batch_ = std::make_unique<TsBatch>();
  }
  ++ts_stats_.batches;
  if (wake) {
    ts_work_cv_.notify_one();
  }
}

void MetalCommandProcessor::TsBarrier(TsSync reason, std::function<void()> fn) {
  ++ts_stats_.syncs[size_t(reason)];
  const uint32_t index = uint32_t(ts_batch_->functions.size());
  ts_batch_->functions.push_back(std::move(fn));
  TsAppend(TsOp::kFunction, {index});
  const uint64_t seq = ts_next_seq_;  // the batch TsPublish is about to queue
  TsPublish();
  const uint64_t t0 = MonoNs();
  {
    std::unique_lock<std::mutex> lock(ts_mutex_);
    ts_done_cv_.wait(lock, [&] { return ts_done_seq_ >= seq; });
  }
  ts_stats_.barrier_wait_ns += MonoNs() - t0;
}

void MetalCommandProcessor::TsAppend(TsOp op, std::initializer_list<uint32_t> words) {
  std::vector<uint32_t>& ops = ts_batch_->ops;
  ops.push_back(uint32_t(op));
  ops.insert(ops.end(), words.begin(), words.end());
}

void MetalCommandProcessor::TsLogRegister(uint32_t index) {
  ++ts_stats_.register_ops;
  TsAppend(TsOp::kRegister, {index, CommandProcessor::register_file_->values[index]});
  if (index == XE_GPU_REG_VGT_EVENT_INITIATOR) {
    // Fences and other events follow: let stage 2 start on what came before.
    TsPublish();
  }
}

void MetalCommandProcessor::TsLogRegisterRange(uint32_t start, const uint32_t* big_endian,
                                               uint32_t count) {
  ++ts_stats_.register_ops;
  std::vector<uint32_t>& ops = ts_batch_->ops;
  ops.push_back(uint32_t(TsOp::kRegisterRange));
  ops.push_back(start);
  ops.push_back(count);
  const size_t at = ops.size();
  ops.resize(at + count);
  std::memcpy(ops.data() + at, big_endian, count * sizeof(uint32_t));
}

void MetalCommandProcessor::TraceRegistersRestored(uint32_t first, uint32_t count) {
  if (!ts_register_file_ || !TsOnStage1() || first >= RegisterFile::kRegisterCount) {
    return;
  }
  count = uint32_t(std::min<size_t>(count, RegisterFile::kRegisterCount - first));
  if (!ts_active_) {
    std::memcpy(register_file_->values + first, CommandProcessor::register_file_->values + first,
                count * sizeof(uint32_t));
    return;
  }
  std::vector<uint32_t>& ops = ts_batch_->ops;
  ops.push_back(uint32_t(TsOp::kRegisterCopy));
  ops.push_back(first);
  ops.push_back(count);
  ops.insert(ops.end(), CommandProcessor::register_file_->values + first,
             CommandProcessor::register_file_->values + first + count);
}

void MetalCommandProcessor::TraceDrainBackend() {
  if (!ts_active_ || !TsOnStage1()) {
    return;
  }
  TsBarrier(TsSync::kTraceDrain, [] {});
  TsLogStats("trace drain");
}

void MetalCommandProcessor::PrepareForIdleWait() {
  if (ts_active_ && TsOnStage1()) {
    // The ring is empty: stage 2 submits what it has without stage 1 waiting.
    TsAppend(TsOp::kIdleWait, {});
    TsPublish();
    return;
  }
  PrepareForWait();
}

void MetalCommandProcessor::OnInterruptPacket() {
  if (!ts_active_ || !TsOnStage1()) {
    return;
  }
  if (ts_interrupt_barrier_) {
    TsBarrier(TsSync::kInterrupt, [] {});
  } else {
    TsPublish();
  }
}

bool MetalCommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  if (ts_active_ && TsOnStage1()) {
    return TsRecordDraw(primitive_type, index_count, index_buffer_info, major_mode_explicit);
  }
  return IssueDrawBackend(primitive_type, index_count, index_buffer_info, major_mode_explicit);
}

bool MetalCommandProcessor::TsRecordDraw(xenos::PrimitiveType primitive_type,
                                         uint32_t index_count,
                                         IndexBufferInfo* index_buffer_info,
                                         bool major_mode_explicit) {
  const RegisterFile& regs = *CommandProcessor::register_file_;
  Shader* vs = CommandProcessor::active_vertex_shader();
  Shader* ps = CommandProcessor::active_pixel_shader();
  IndexBufferInfo ibi;
  if (index_buffer_info) {
    ibi = *index_buffer_info;
  }
  const bool indexed = index_buffer_info != nullptr;

  ts_ranges_.clear();
  uint32_t alias_offset = kNoAlias;
  TsSync sync = TsSync::kCount;
  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (ts_sync_all_draws_) {
    // Bisect knob: every draw is a barrier that uploads from live guest RAM.
    sync = TsSync::kMaintenance;
  } else if (edram_mode == xenos::EdramMode::kCopy && ts_sync_resolves_) {
    sync = TsSync::kMaintenance;  // bisect knob
  } else if (edram_mode == xenos::EdramMode::kCopy) {
    // A resolve: it reads the destination's partial pages, and later draws
    // that sample it must not snapshot the stale CPU bytes over its output.
    draw_util::ResolveInfo info;
    if (render_target_cache_ &&
        draw_util::GetResolveInfo(regs, *memory_, trace_writer_,
                                  render_target_cache_->draw_resolution_scale_x(),
                                  render_target_cache_->draw_resolution_scale_y(),
                                  render_target_cache_->IsFixedRG16TruncatedToMinus1To1(),
                                  render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1(),
                                  info) &&
        info.copy_dest_extent_length) {
      SharedMemory::Range range{info.copy_dest_extent_start, info.copy_dest_extent_length};
      const uint32_t first_run = uint32_t(ts_batch_->runs.size());
      if (!shared_memory_->CaptureRanges(&range, 1, ts_batch_->arena, ts_batch_->runs)) {
        ++ts_stats_.capture_failures;
      }
      for (size_t i = first_run; i < ts_batch_->runs.size(); ++i) {
        ++ts_stats_.captured_runs;
        ts_stats_.captured_bytes += ts_batch_->runs[i].length;
      }
      shared_memory_->MarkRangeGpuWrittenForCapture(range.start, range.length);
    }
  } else if (vs) {
    if (!vs->is_ucode_analyzed()) {
      vs->AnalyzeUcode(ts_ucode_disasm_buffer_);
    }
    if (ps && !ps->is_ucode_analyzed()) {
      ps->AnalyzeUcode(ts_ucode_disasm_buffer_);
    }
    const bool tessellated =
        major_mode_explicit && regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
                                   xenos::VGTOutputPath::kTessellationEnable;
    if (vs->memexport_eM_written() || (ps && ps->memexport_eM_written())) {
      sync = TsSync::kMemexport;
    } else if (tessellated) {
      sync = TsSync::kTessellation;
    } else if (indexed && primitive_type != xenos::PrimitiveType::kTriangleList &&
               primitive_type != xenos::PrimitiveType::kLineList) {
      // Index conversion / primitive reset read guest indices on the CPU.
      sync = TsSync::kIndexConversion;
    } else {
      uint32_t vertex_count = 0;
      ts_ranges_.resize(96 + 2 + 2 * 64);
      if (!CollectVertexFetchRangesNoLog(regs, *vs, ts_ranges_.data(), 96, &vertex_count)) {
        vertex_count = 0;  // the backend drops the draw and logs why
      }
      ts_ranges_.resize(vertex_count);
      if (indexed && ibi.guest_base) {
        const uint32_t stride = ibi.format == xenos::IndexFormat::kInt16 ? 2 : 4;
        ts_ranges_.push_back({ibi.guest_base, ibi.count * stride});
      }
      SharedMemory::Range texture_ranges[2];
      auto add_textures = [&](const Shader& shader) {
        for (const Shader::TextureBinding& binding : shader.texture_bindings()) {
          const uint32_t n = MetalTextureCache::GuestRangesForFetchConstant(
              regs.GetTextureFetch(binding.fetch_constant), texture_ranges);
          for (uint32_t i = 0; i < n; ++i) {
            ts_ranges_.push_back(texture_ranges[i]);
          }
        }
      };
      add_textures(*vs);
      if (ps) {
        add_textures(*ps);
      }
      // The XDK clear alias reads its 21 vertex dwords on the CPU.
      const auto& bindings = vs->vertex_bindings();
      if (primitive_type == xenos::PrimitiveType::kRectangleList && bindings.size() == 1 &&
          bindings[0].stride_words == 7) {
        xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(bindings[0].fetch_constant);
        const void* vdata = memory_->TranslatePhysical(uint32_t(vf.address) << 2);
        if (vf.type == xenos::FetchConstantType::kVertex && vf.size >= 21 && vdata) {
          std::vector<uint8_t>& arena = ts_batch_->arena;
          arena.resize((arena.size() + 3) & ~size_t(3));
          alias_offset = uint32_t(arena.size());
          arena.resize(arena.size() + 21 * sizeof(uint32_t));
          std::memcpy(arena.data() + alias_offset, vdata, 21 * sizeof(uint32_t));
        }
      }
      const uint32_t first_run = uint32_t(ts_batch_->runs.size());
      if (!ts_ranges_.empty() &&
          !shared_memory_->CaptureRanges(ts_ranges_.data(), uint32_t(ts_ranges_.size()),
                                         ts_batch_->arena, ts_batch_->runs)) {
        ++ts_stats_.capture_failures;
      }
      for (size_t i = first_run; i < ts_batch_->runs.size(); ++i) {
        ++ts_stats_.captured_runs;
        ts_stats_.captured_bytes += ts_batch_->runs[i].length;
      }
    }
  }

  if (sync != TsSync::kCount) {
    ++ts_stats_.draws_sync;
    bool result = true;
    TsBarrier(sync, [&, this] {
      ts_vertex_shader_ = vs;
      ts_pixel_shader_ = ps;
      result = IssueDrawBackend(primitive_type, index_count, indexed ? &ibi : nullptr,
                                major_mode_explicit);
    });
    return result;
  }

  // Runs captured by this draw: [first, end) of batch->runs.
  uint32_t run_end = uint32_t(ts_batch_->runs.size());
  uint32_t flags = (indexed ? kDrawIndexed : 0) | (major_mode_explicit ? kDrawMajorModeExplicit : 0);
  const uint64_t vs_bits = reinterpret_cast<uintptr_t>(vs);
  const uint64_t ps_bits = reinterpret_cast<uintptr_t>(ps);
  TsAppend(TsOp::kDraw,
           {uint32_t(primitive_type), index_count, flags, uint32_t(ibi.format),
            uint32_t(ibi.endianness), ibi.count, ibi.guest_base, uint32_t(ibi.length),
            uint32_t(vs_bits), uint32_t(vs_bits >> 32), uint32_t(ps_bits),
            uint32_t(ps_bits >> 32), run_end, alias_offset});
  ++ts_stats_.draws_async;
  if (++ts_batch_->draws >= ts_publish_draws_) {
    TsPublish();
  }
  // Failures are reported by stage 2 (the base only logs a false result).
  return true;
}

void MetalCommandProcessor::TsExecuteBatch(TsBatch& batch) {
  const uint32_t* op = batch.ops.data();
  const uint32_t* end = op + batch.ops.size();
  uint32_t next_run = 0;
  // Outside a barrier stage 1 has run ahead: live guest RAM is newer than the
  // ops executing here, so the shared-memory funnel must never upload from it.
  ts_async_draw_ = true;
  SharedMemory::SetThreadSnapshotView(true);
  while (op < end) {
    const TsOp kind = TsOp(*op++);
    switch (kind) {
      case TsOp::kRegister: {
        const uint32_t index = op[0];
        ApplyRegisterWrite(index, register_file_->values[index], op[1]);
        op += 2;
        break;
      }
      case TsOp::kRegisterRange: {
        const uint32_t start = op[0];
        const uint32_t count = op[1];
        uint32_t* words = const_cast<uint32_t*>(op + 2);
        if (!TryWriteKnownRegisterRangeFromMem(start, words, count)) {
          for (uint32_t i = 0; i < count; ++i) {
            ApplyRegisterWrite(start + i, register_file_->values[start + i],
                               rex::byte_swap(words[i]));
          }
        }
        op += 2 + count;
        break;
      }
      case TsOp::kRegisterCopy: {
        const uint32_t first = op[0];
        const uint32_t count = op[1];
        std::memcpy(register_file_->values + first, op + 2, count * sizeof(uint32_t));
        op += 2 + count;
        break;
      }
      case TsOp::kDraw: {
        const uint32_t run_end = op[12];
        // Draws still in the prepared-draw queue read the shared buffer when
        // the queue flushes; encode them before this draw's snapshot
        // overwrites pages they use.
        if (next_run < run_end && !prepared_draw_queue_.empty() &&
            (ts_flush_queue_always_ || TsRunsOverlapPreparedQueue(batch, next_run, run_end))) {
          ++ts_stats_.overlap_flushes;
          if (!FlushPreparedDrawQueue(PreparedDrawFlushReason::kManual)) {
            REXLOG_ERROR("[cp-two-stage] prepared-draw queue flush failed");
          }
        }
        for (; next_run < run_end; ++next_run) {
          if (!shared_memory_->UploadCapturedRun(batch.runs[next_run], batch.arena.data())) {
            ++ts_stats_.upload_failures;
          }
        }
        IndexBufferInfo ibi;
        ibi.format = xenos::IndexFormat(op[3]);
        ibi.endianness = xenos::Endian(op[4]);
        ibi.count = op[5];
        ibi.guest_base = op[6];
        ibi.length = op[7];
        ts_vertex_shader_ =
            reinterpret_cast<Shader*>(uintptr_t(op[8]) | (uint64_t(op[9]) << 32));
        ts_pixel_shader_ =
            reinterpret_cast<Shader*>(uintptr_t(op[10]) | (uint64_t(op[11]) << 32));
        ts_alias_clear_vertices_ =
            op[13] != kNoAlias
                ? reinterpret_cast<const uint32_t*>(batch.arena.data() + op[13])
                : nullptr;
        ++ts_stats_.stage2_draws;
        const bool ok = IssueDrawBackend(xenos::PrimitiveType(op[0]), op[1],
                                         (op[2] & kDrawIndexed) ? &ibi : nullptr,
                                         (op[2] & kDrawMajorModeExplicit) != 0);
        ts_alias_clear_vertices_ = nullptr;
        if (!ok) {
          REXLOG_ERROR("[cp-two-stage] queued draw failed in backend (prim {}, {} indices)",
                       op[0], op[1]);
        }
        op += kDrawWords;
        break;
      }
      case TsOp::kZpd:
        ZpdExecuteEventTail(op[0], op[1] != 0, true);
        op += 2;
        break;
      case TsOp::kPrimaryBufferEnd:
        OnPrimaryBufferEnd();
        break;
      case TsOp::kIdleWait:
        PrepareForWait();
        break;
      case TsOp::kReturnFromWait:
        ReturnFromWait();
        break;
      case TsOp::kFunction: {
        // Stage 1 is parked until this batch completes: the closure may use
        // the guest register file and live guest memory.
        ts_in_barrier_ = true;
        ts_async_draw_ = false;
        SharedMemory::SetThreadSnapshotView(false);
        batch.functions[op[0]]();
        if (ts_verify_registers_) {
          uint32_t mismatches = 0;
          uint32_t first_index = 0;
          for (uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
            if (register_file_->values[i] != CommandProcessor::register_file_->values[i]) {
              if (!mismatches++) {
                first_index = i;
              }
            }
          }
          if (mismatches) {
            std::fprintf(stderr,
                         "[cp-two-stage] verify: %u register(s) differ, first 0x%04X "
                         "(guest %08X, stage 2 %08X)\n",
                         mismatches, first_index,
                         CommandProcessor::register_file_->values[first_index],
                         register_file_->values[first_index]);
          }
        }
        ts_in_barrier_ = false;
        ts_async_draw_ = true;
        SharedMemory::SetThreadSnapshotView(true);
        op += 1;
        break;
      }
    }
  }
  ts_async_draw_ = false;
  SharedMemory::SetThreadSnapshotView(false);
}

bool MetalCommandProcessor::TsRunsOverlapPreparedQueue(const TsBatch& batch, uint32_t first,
                                                       uint32_t end) const {
  for (const PreparedDrawPtr& draw : prepared_draw_queue_) {
    for (const SharedMemory::Range& range : draw->materialization_ranges) {
      const uint64_t range_end = uint64_t(range.start) + range.length;
      for (uint32_t i = first; i < end; ++i) {
        const SharedMemory::CapturedRun& run = batch.runs[i];
        if (run.start < range_end && range.start < uint64_t(run.start) + run.length) {
          return true;
        }
      }
    }
  }
  return false;
}

void MetalCommandProcessor::TsNoteStage2Upload(uint32_t reason, uint64_t bytes) {
  ++ts_stats_.misses;
  ts_stats_.miss_bytes += bytes;
  ++ts_stats_.miss_by_reason[std::min<uint32_t>(reason, 15)];
}

void MetalCommandProcessor::TsLogStats(const char* when) {
  const TsStats& s = ts_stats_;
  std::string syncs;
  for (size_t i = 0; i < size_t(TsSync::kCount); ++i) {
    if (s.syncs[i]) {
      syncs += fmt::format(" {}={}", kSyncNames[i], s.syncs[i]);
    }
  }
  std::string misses;
  for (size_t i = 0; i < 16; ++i) {
    if (s.miss_by_reason[i]) {
      misses += fmt::format(" r{}={}", i, s.miss_by_reason[i]);
    }
  }
  std::fprintf(stderr,
               "[cp-two-stage] %s: batches %llu (stage2 %llu), draws async %llu sync %llu, "
               "reg ops %llu, captured %llu runs %llu bytes (fail %llu, upload fail %llu), "
               "misses %llu / %llu bytes%s, barriers:%s, barrier wait %.3f ms, "
               "stage2 cpu %.3f ms, queue overlap flushes %llu\n",
               when, (unsigned long long)s.batches, (unsigned long long)s.stage2_batches,
               (unsigned long long)s.draws_async, (unsigned long long)s.draws_sync,
               (unsigned long long)s.register_ops, (unsigned long long)s.captured_runs,
               (unsigned long long)s.captured_bytes, (unsigned long long)s.capture_failures,
               (unsigned long long)s.upload_failures, (unsigned long long)s.misses,
               (unsigned long long)s.miss_bytes, misses.c_str(), syncs.c_str(),
               s.barrier_wait_ns / 1e6, s.stage2_cpu_ns / 1e6, (unsigned long long)s.overlap_flushes);
  ts_stats_ = TsStats();
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
