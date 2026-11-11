/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_shared_memory.h"

#include <algorithm>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "rex/cvar.h"
#include "rex/logging.h"
#include "rex/math.h"
#include "rex/memory/utils.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/hash.h"

DEFINE_bool(
    metal_shared_memory_direct_write, true,
    "Directly copy safe Metal shared-memory uploads into the shared storage "
    "buffer instead of staging them through a blit.",
    "Metal");

namespace rex {
namespace graphics {
namespace metal {

namespace {
constexpr uint64_t kPreservedExportLifetimeUs = 150000;  // ~9 frames at 60 FPS
constexpr size_t kPreservedExportMaxEntries = 256;
constexpr uint64_t kPreservedExportMaxBytes = 32ull << 20;
constexpr uint32_t kPreservedExportMaxRangeBytes = 1u << 20;
uint64_t PreserveNowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
// [memexport-hole] REX_METAL_MEMEXPORT_PENDING_KEEP=1: an upload leaves out
// every export range whose readback is still pending and whose guest bytes
// are unchanged since the export draw (guest RAM is older than the buffer).
bool MemexportPendingKeepEnabled() {
  static const bool enabled = [] {
    const char* v = MetalDebugEnv("MEMEXPORT_PENDING_KEEP");
    const bool on = v && v[0] == '1';
    REXLOG_INFO("[memexport-hole] pending-keep {} "
                "(REX_METAL_MEMEXPORT_PENDING_KEEP={})",
                on ? "on" : "off", v ? v : "unset");
    return on;
  }();
  return enabled;
}
bool PreserveTraceEnabled() {
  static const bool enabled = [] {
    const char* v = MetalDebugEnv("DRAW_SKIP_TRACE");
    return v && v[0] == '1';
  }();
  return enabled;
}
}  // namespace

UploadTraceCounters& UploadTrace() {
  static UploadTraceCounters counters;  // GPU Commands thread only
  return counters;
}

bool MetalSharedMemory::UploadPrecommitEnabled() {
  static const bool enabled = [] {
    const char* v = MetalDebugEnv("UPLOAD_PRECOMMIT");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[upload-precommit] {} (FABLE2_UPLOAD_PRECOMMIT={})",
        on ? "on: staged uploads of pages last read only by committed command "
             "buffers go to a command buffer committed ahead of the current "
             "one instead of ending the render pass"
           : "off: every staged upload is encoded in the current command buffer",
        v ? v : "unset");
    return on;
  }();
  return enabled;
}

MetalSharedMemory::PageAccess MetalSharedMemory::ClassifyPageAccessLocked(
    uint32_t page, uint64_t completed, uint64_t committed,
    uint64_t* last_access) const {
  uint64_t access = 0;
  if (page < page_last_main_gpu_access_submission_.size()) {
    access = page_last_main_gpu_access_submission_[page];
  }
  if (last_access) {
    *last_access = access;
  }
  if (page < page_standalone_gpu_access_counts_.size() &&
      page_standalone_gpu_access_counts_[page]) {
    return PageAccess::kStandalone;
  }
  if (access <= completed) {
    return PageAccess::kCompleted;
  }
  if (access <= committed) {
    return PageAccess::kInflight;
  }
  return PageAccess::kCurrent;
}

bool MetalSharedMemory::MemexportPreserveEnabled() {
  static const bool enabled = [] {
    // Default on (2026-10-09); FABLE2_MEMEXPORT_PRESERVE=0 disables.
    const char* v = MetalDebugEnv("MEMEXPORT_PRESERVE");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[memexport-preserve] {} (FABLE2_MEMEXPORT_PRESERVE={})",
        on ? "on: re-uploads keep GPU-written memexport data unless the "
             "guest bytes changed"
           : "off",
        v ? v : "unset");
    return on;
  }();
  return enabled;
}

void MetalSharedMemory::ExpirePreservedExports(uint64_t now_us) {
  preserved_exports_.erase(
      std::remove_if(preserved_exports_.begin(), preserved_exports_.end(),
                     [now_us](const PreservedExport& e) {
                       return now_us - e.last_export_us >
                              kPreservedExportLifetimeUs;
                     }),
      preserved_exports_.end());
}

void MetalSharedMemory::NoteMemexportWrite(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(start) + length, kBufferSize));
  static uint32_t logged = 0;
  if (logged < 40) {
    ++logged;
    REXLOG_INFO("[memexport-preserve] export range 0x{:08X} len={} entries={}",
                start, end - start, preserved_exports_.size());
  }
  if (end - start > kPreservedExportMaxRangeBytes) {
    return;  // keep hashing bounded; huge exports are not preserved
  }
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  if (!guest) {
    return;
  }
  const uint64_t now_us = PreserveNowUs();
  ExpirePreservedExports(now_us);
  const uint64_t hash = XXH3_64bits(guest + start, end - start);
  for (PreservedExport& e : preserved_exports_) {
    if (e.start == start && e.end == end) {
      e.guest_hash = hash;
      e.last_export_us = now_us;
      return;
    }
  }
  // Overlapping but different ranges: drop the older ones (keep it simple).
  preserved_exports_.erase(
      std::remove_if(preserved_exports_.begin(), preserved_exports_.end(),
                     [start, end](const PreservedExport& e) {
                       return e.start < end && start < e.end;
                     }),
      preserved_exports_.end());
  uint64_t total = end - start;
  for (const PreservedExport& e : preserved_exports_) total += e.end - e.start;
  if (preserved_exports_.size() >= kPreservedExportMaxEntries ||
      total > kPreservedExportMaxBytes) {
    return;
  }
  preserved_exports_.push_back({start, end, hash, now_us});
}

bool MetalSharedMemory::ShmemValidFirstEnabled() {
  // CR-02 (docs/cr-fixes-20261007.md). On unless FABLE2_SHMEM_VALID_FIRST=0;
  // the Dev Launcher always passes an explicit value.
  static const bool enabled = [] {
    const char* v = MetalDebugEnv("SHMEM_VALID_FIRST");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO("[shmem-valid-first] {} (FABLE2_SHMEM_VALID_FIRST={})",
                on ? "on: direct writes mark pages valid before copying"
                   : "off: direct writes copy, then mark valid",
                v ? v : "unset");
    return on;
  }();
  return enabled;
}

bool MetalSharedMemory::MemexportReadbackEnabled() {
  static const bool enabled = [] {
    // Default on (2026-10-09); FABLE2_MEMEXPORT_READBACK=0 disables.
    const char* v = MetalDebugEnv("MEMEXPORT_READBACK");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[memexport-readback] {} (FABLE2_MEMEXPORT_READBACK={})",
        on ? "on: memexport ranges are copied back from the GPU buffer to "
             "guest RAM after their command buffer completes"
           : "off",
        v ? v : "unset");
    return on;
  }();
  return enabled;
}

bool MetalSharedMemory::MemexportMergeEnabled() {
  static const bool enabled = [] {
    const char* v = MetalDebugEnv("MEMEXPORT_MERGE");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO("[memexport-merge] {} (REX_METAL_MEMEXPORT_MERGE={})",
                on ? "on: a readback copies back the words its export "
                     "changed, even when the CPU wrote elsewhere in the range"
                   : "off (count only)",
                v ? v : "unset");
    return on;
  }();
  return enabled;
}

bool MetalSharedMemory::MemexportGuardEnabled() {
  static const bool enabled = [] {
    const char* v = MetalDebugEnv("MEMEXPORT_GUARD");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[memexport-guard] {} (MEMEXPORT_GUARD={})",
        on ? "on: pages the CPU wrote around a memexport draw stay invalid "
             "and are not read back"
           : "off",
        v ? v : "unset");
    return on;
  }();
  return enabled;
}

void MetalSharedMemory::MemexportRangeWritten(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page = uint32_t(1) << page_size_log2();
  const uint32_t end = start + length;
  ++guard_draw_ranges_;
  // The range was requested (uploaded) for this draw, so an invalid page here
  // was written by the CPU since then.
  uint32_t invalid_pages = 0;
  for (uint32_t p = start & ~(page - 1); p < end; p += page) {
    if (!IsRangeValid(p, page)) {
      ++invalid_pages;
    }
  }
  if (!invalid_pages) {
    RangeWrittenByGpu(start, length);
    return;
  }
  ++guard_draw_ranges_hit_;
  guard_draw_pages_invalid_ += invalid_pages;
  if (guard_draw_ranges_hit_ <= 16) {
    REXLOG_WARN(
        "[memexport-guard] draw #{}: range {:08X}+{} has {} of {} pages "
        "invalidated by a CPU write since its upload",
        guard_draw_ranges_hit_, start, length, invalid_pages,
        ((end - 1) >> page_size_log2()) - (start >> page_size_log2()) + 1);
  }
  if (!MemexportGuardEnabled()) {
    RangeWrittenByGpu(start, length);
    return;
  }
  uint32_t run = UINT32_MAX;
  for (uint32_t p = start & ~(page - 1);; p += page) {
    const bool valid = p < end && IsRangeValid(p, page);
    if (valid) {
      if (run == UINT32_MAX) {
        run = std::max(p, start);
      }
    } else if (run != UINT32_MAX) {
      RangeWrittenByGpu(run, std::min(p, end) - run);
      run = UINT32_MAX;
    }
    if (p >= end) {
      break;
    }
  }
}

namespace {
constexpr uint32_t kReadbackMaxRangeBytes = 8u << 20;
constexpr size_t kReadbackMaxPending = 4096;
constexpr uint32_t kReadbackSnapshotMaxBytes = 2u << 20;
}  // namespace

void MetalSharedMemory::NoteMemexportReadback(uint32_t start, uint32_t length,
                                              uint64_t submission) {
  if (!length || start >= kBufferSize || !submission) {
    return;
  }
  uint32_t end =
      uint32_t(std::min<uint64_t>(uint64_t(start) + length, kBufferSize));
  if (end - start > kReadbackMaxRangeBytes) {
    static uint32_t logged_big = 0;
    if (logged_big < 8) {
      ++logged_big;
      REXLOG_WARN("[memexport-readback] range 0x{:08X} len={} too large, "
                  "not read back",
                  start, end - start);
    }
    return;
  }
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  if (!guest) {
    return;
  }
  if (pending_readbacks_.size() >= kReadbackMaxPending) {
    // Should not happen (completions are processed every command buffer);
    // drop the oldest so bookkeeping stays bounded.
    pending_readbacks_.pop_front();
  }
  pending_readbacks_.push_back({start, end, submission,
                                XXH3_64bits(guest + start, end - start),
                                frame_log::CurrentFrame(), {}});
  if (end - start <= kReadbackSnapshotMaxBytes) {
    pending_readbacks_.back().before.assign(guest + start, guest + end);
  }
  pending_readbacks_.back().export_us = PreserveNowUs();
  pending_readbacks_.back().seq = ++readback_seq_;
}

void MetalSharedMemory::NoteMemexportShadow(MemexportShadow&& shadow,
                                            uint64_t submission,
                                            uint32_t own_ranges) {
  if (shadow.end <= shadow.start || shadow.end > kBufferSize) {
    ++shadow_dropped_;
    return;
  }
  shadow.submission = submission;
  shadow.seq_last = readback_seq_;
  shadow.seq_first = readback_seq_ >= own_ranges
                         ? readback_seq_ - own_ranges + 1
                         : 1;
  for (const PendingReadback& r : pending_readbacks_) {
    if (r.seq >= shadow.seq_first) {
      continue;
    }
    if (r.start < shadow.src_end && shadow.src_start < r.end) {
      shadow.src_pending = true;
      break;
    }
  }
  if (shadow.src_end > shadow.src_start && shadow.src_end <= kBufferSize) {
    shadow.src_invalid =
        !IsRangeValid(shadow.src_start, shadow.src_end - shadow.src_start);
  }
  if (memexport_shadows_.size() >= 4096) {
    memexport_shadows_.pop_front();
    ++shadow_dropped_;
  }
  shadow_dup_copies_ += !shadow.losers.empty();
  shadow_dup_rewritten_words_ += shadow.rewritten_words;
  shadow_dup_contested_words_ += shadow.losers.size();
  memexport_shadows_.push_back(std::move(shadow));
}

void MetalSharedMemory::NoteMemexportSourceState(MemexportShadow& shadow) {
  if (shadow.src_end <= shadow.src_start || shadow.src_end > kBufferSize ||
      page_last_gpu_write_submission_.empty()) {
    return;
  }
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  const uint32_t psl = page_size_log2();
  for (uint32_t page = shadow.src_start >> psl;
       page <= (shadow.src_end - 1) >> psl; ++page) {
    ++shadow.src_pages;
    shadow.src_pages_invalid += !IsRangeValid(page << psl, 1u << psl);
    std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
    shadow.src_pages_gpu_write +=
        page_last_gpu_write_submission_[page] > completed;
    shadow.src_pages_staged +=
        page_last_staged_copy_submission_[page] > completed;
    shadow.src_pages_precommit +=
        page_last_precommit_copy_submission_[page] > completed;
    shadow.src_pages_standalone +=
        page_standalone_gpu_access_counts_[page] != 0;
  }
  for (const PendingReadback& r : pending_readbacks_) {
    shadow.src_readbacks_pending +=
        r.start < shadow.src_end && shadow.src_start < r.end;
  }
}

void MetalSharedMemory::NoteMemexportDestinationState(
    MemexportShadow& shadow) {
  if (shadow.end <= shadow.start || shadow.end > kBufferSize || !buffer_ ||
      page_last_gpu_write_submission_.empty()) {
    return;
  }
  const uint8_t* gpu = static_cast<const uint8_t*>(buffer_->contents());
  if (gpu && shadow.end - shadow.start <= (4u << 20)) {
    shadow.dst_buf_before.assign(gpu + shadow.start, gpu + shadow.end);
  }
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  const uint32_t psl = page_size_log2();
  for (uint32_t page = shadow.start >> psl; page <= (shadow.end - 1) >> psl;
       ++page) {
    ++shadow.dst_pages;
    const bool invalid = !IsRangeValid(page << psl, 1u << psl);
    shadow.dst_page_invalid.push_back(invalid);
    shadow.dst_pages_invalid += invalid;
    std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
    shadow.dst_pages_gpu_write +=
        page_last_gpu_write_submission_[page] > completed;
    shadow.dst_pages_staged +=
        page_last_staged_copy_submission_[page] > completed;
    shadow.dst_pages_precommit +=
        page_last_precommit_copy_submission_[page] > completed;
  }
  for (const PendingReadback& r : pending_readbacks_) {
    shadow.dst_readbacks_pending += r.start < shadow.end && shadow.start < r.end;
  }
}

uint32_t MetalSharedMemory::CheckMemexportSourceDivergence(
    uint32_t start, uint32_t length, uint32_t destination) {
  if (!length || !buffer_ || uint64_t(start) + length > kBufferSize ||
      page_last_gpu_write_submission_.empty()) {
    return 0;
  }
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  const uint8_t* gpu = static_cast<const uint8_t*>(buffer_->contents());
  if (!guest || !gpu) {
    return 0;
  }
  ++src_div_checks_;
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  const uint32_t psl = page_size_log2();
  const uint32_t end = start + length;
  uint32_t total = 0;
  for (uint32_t page = start >> psl; page <= (end - 1) >> psl; ++page) {
    const uint32_t p0 = std::max(start, page << psl) & ~3u;
    const uint32_t p1 = std::min(end, (page + 1) << psl) & ~3u;
    if (p1 <= p0 || !IsRangeValid(p0, p1 - p0)) {
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
      if (page_last_gpu_write_submission_[page] > completed ||
          page_last_staged_copy_submission_[page] > completed ||
          page_last_precommit_copy_submission_[page] > completed ||
          page_standalone_gpu_access_counts_[page]) {
        continue;
      }
    }
    bool pending = false;
    for (const PendingReadback& r : pending_readbacks_) {
      if (r.start < p1 && p0 < r.end) {
        pending = true;
        break;
      }
    }
    if (pending) {
      continue;
    }
    src_div_settled_bytes_ += p1 - p0;
    if (!std::memcmp(gpu + p0, guest + p0, p1 - p0)) {
      continue;
    }
    // A guest write in progress makes the page invalid before it lands.
    if (!IsRangeValid(p0, p1 - p0)) {
      continue;
    }
    uint32_t words = 0, guest_zero = 0, buffer_zero = 0, first = 0;
    for (uint32_t a = p0; a < p1; a += 4) {
      uint32_t g, c;
      std::memcpy(&g, gpu + a, 4);
      std::memcpy(&c, guest + a, 4);
      if (g != c) {
        if (!words) {
          first = a;
        }
        ++words;
        guest_zero += c == 0;
        buffer_zero += g == 0;
      }
    }
    if (!words) {
      continue;
    }
    total += words;
    ++src_div_pages_;
    src_div_words_ += words;
    src_div_words_guest_zero_ += guest_zero;
    src_div_words_buffer_zero_ += buffer_zero;
    if (src_div_logged_ < 200) {
      ++src_div_logged_;
      uint32_t g, c;
      std::memcpy(&g, gpu + first, 4);
      std::memcpy(&c, guest + first, 4);
      REXLOG_WARN(
          "[shmem-diverge] #{}: page {:08X}-{:08X} (source {:08X}+{} of a "
          "copy to {:08X}, completed submission {}) is valid and settled but "
          "{} of {} words differ, first at {:08X}: buffer {:08X} guest "
          "{:08X}; differing words that are zero in guest {}, in the buffer "
          "{}",
          src_div_logged_, p0, p1, start, length, destination, completed,
          words, (p1 - p0) / 4, first, g, c, guest_zero, buffer_zero);
    }
  }
  return total;
}

void MetalSharedMemory::CheckCompletedMemexportShadows(
    uint64_t completed_submission) {
  if (memexport_shadows_.empty() ||
      memexport_shadows_.front().submission > completed_submission ||
      !buffer_) {
    return;
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  const uint8_t* gpu = static_cast<const uint8_t*>(buffer_->contents());
  if (!guest || !gpu) {
    return;
  }
  while (!memexport_shadows_.empty() &&
         memexport_shadows_.front().submission <= completed_submission) {
    const MemexportShadow& s = memexport_shadows_.front();
    bool later_export = false;
    bool hole = false;
    for (const PendingReadback& r : pending_readbacks_) {
      if (r.seq >= s.seq_first && r.seq <= s.seq_last) {
        hole |= r.hole;
        continue;
      }
      if (r.seq > s.seq_last && r.start < s.end && s.start < r.end) {
        later_export = true;
      }
    }
    const uint32_t len = s.end - s.start;
    uint64_t ok = 0, later = 0, cpu_later = 0, missing = 0, wrong = 0;
    uint32_t first_bad = UINT32_MAX;
    bool first_bad_missing = false;
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
      if (!s.written[i]) {
        continue;
      }
      uint32_t g, e, b, c;
      std::memcpy(&g, gpu + s.start + i, 4);
      std::memcpy(&e, s.expected.data() + i, 4);
      if (g == e) {
        ++ok;
        continue;
      }
      std::memcpy(&b, s.before.data() + i, 4);
      std::memcpy(&c, guest + s.start + i, 4);
      if (later_export) {
        ++later;
      } else if (g == b) {
        ++missing;
        if (first_bad == UINT32_MAX) {
          first_bad = i;
          first_bad_missing = true;
        }
      } else if (g == c) {
        ++cpu_later;
      } else {
        ++wrong;
        if (first_bad == UINT32_MAX) {
          first_bad = i;
        }
      }
    }
    ++shadow_draws_;
    shadow_words_ok_ += ok;
    shadow_words_later_export_ += later;
    shadow_words_cpu_later_ += cpu_later;
    shadow_words_missing_ += missing;
    shadow_words_wrong_ += wrong;
    if (s.src_pending) {
      ++shadow_draws_src_pending_;
    }
    if (s.src_invalid) {
      ++shadow_draws_src_invalid_;
    }
    if (hole) {
      ++shadow_draws_hole_;
    }
    if (s.src_pending || s.src_invalid) {
      shadow_words_wrong_src_ += wrong + missing;
    }
    if (missing || wrong) {
      ++shadow_draws_bad_;
      if (!s.src_pending) {
        ++shadow_draws_bad_nopend_;
      }
      if (s.src_div_words) {
        ++shadow_draws_bad_div_;
      }
      if (!s.src_pending && !s.src_div_words) {
        ++shadow_draws_bad_unexplained_;
        shadow_words_bad_unexplained_ += missing + wrong;
      }
      if (shadow_logged_ < 200 && !s.src_pending) {
        ++shadow_logged_;
        uint32_t g = 0, e = 0, b = 0, c = 0;
        std::memcpy(&g, gpu + s.start + first_bad, 4);
        std::memcpy(&e, s.expected.data() + first_bad, 4);
        std::memcpy(&b, s.before.data() + first_bad, 4);
        std::memcpy(&c, guest + s.start + first_bad, 4);
        REXLOG_WARN(
            "[memexport-shadow] #{}: copy to {:08X}+{} from {:08X}+{} "
            "(submission {}, completed {}): words ok {}, missing {}, wrong "
            "{}, later CPU write {}; source pending {}, source not valid {}, "
            "upload went over it {}; first bad word at {:08X} ({}): buffer "
            "{:08X} expected {:08X} before the copy {:08X} guest now {:08X}; "
            "settled source words where buffer and guest differed {}",
            shadow_logged_, s.start, len, s.src_start,
            s.src_end - s.src_start, s.submission, completed_submission, ok,
            missing, wrong, cpu_later, s.src_pending, s.src_invalid, hole,
            s.start + first_bad, first_bad_missing ? "missing" : "wrong", g,
            e, b, c, s.src_div_words);
      }
      // What the source did between the model and the completed copy.
      uint64_t src_guest_changed = 0, src_buffer_differs = 0;
      uint64_t as_buffer = 0, as_guest = 0, as_neither = 0;
      const bool have_src =
          !s.src_snap.empty() && s.src_end <= kBufferSize &&
          s.src_snap.size() == size_t(s.src_end - s.src_start);
      if (have_src) {
        for (size_t i = 0; i + 4 <= s.src_snap.size(); i += 4) {
          src_guest_changed += std::memcmp(guest + s.src_start + i,
                                           s.src_snap.data() + i, 4) != 0;
          src_buffer_differs += std::memcmp(gpu + s.src_start + i,
                                            s.src_snap.data() + i, 4) != 0;
        }
      }
      if (s.rerun) {
        std::vector<uint8_t> from_buffer = s.before;
        std::vector<uint8_t> from_guest = s.before;
        s.rerun(gpu, from_buffer);
        s.rerun(guest, from_guest);
        for (uint32_t i = 0; i + 4 <= len; i += 4) {
          if (!s.written[i] ||
              !std::memcmp(gpu + s.start + i, s.expected.data() + i, 4)) {
            continue;
          }
          if (!std::memcmp(gpu + s.start + i, from_buffer.data() + i, 4)) {
            ++as_buffer;
          } else if (!std::memcmp(gpu + s.start + i, from_guest.data() + i,
                                  4)) {
            ++as_guest;
          } else {
            ++as_neither;
          }
        }
      }
      // Bad words that hold a value an earlier vertex of the same draw
      // wrote there (the CPU model lets the last vertex win).
      uint64_t bad_words = 0, as_loser = 0;
      for (uint32_t i = 0; i + 4 <= len; i += 4) {
        if (!s.written[i] ||
            !std::memcmp(gpu + s.start + i, s.expected.data() + i, 4)) {
          continue;
        }
        ++bad_words;
        uint32_t g;
        std::memcpy(&g, gpu + s.start + i, 4);
        auto it = std::lower_bound(
            s.losers.begin(), s.losers.end(),
            std::pair<uint32_t, uint32_t>(i, 0));
        for (; it != s.losers.end() && it->first == i; ++it) {
          if (it->second == g) {
            ++as_loser;
            break;
          }
        }
      }
      shadow_bad_words_all_ += bad_words;
      shadow_dup_bad_words_ += as_loser;
      shadow_dup_bad_copies_ += as_loser != 0;
      if (!s.src_pending) {
        shadow_bad_words_not_pending_ += bad_words;
        shadow_dup_bad_words_not_pending_ += as_loser;
      }
      if (shadow_dup_logged_ < 120 && (as_loser || !s.src_pending)) {
        ++shadow_dup_logged_;
        REXLOG_WARN(
            "[memexport-shadow-dup] copy to {:08X}+{} (submission {}, "
            "source pending {}): bad words {}, of them equal to a value "
            "an earlier vertex of the same draw wrote there {}; words "
            "the draw writes more than once {}, with different values "
            "{}",
            s.start, len, s.submission, s.src_pending, bad_words, as_loser,
            s.rewritten_words, s.losers.size());
      }
      if (!s.src_pending &&
          (as_neither || (!src_guest_changed && !src_buffer_differs))) {
        // Destination side of a bad copy the source does not explain.
        const uint32_t psl = page_size_log2();
        const uint32_t page_mask = (1u << psl) - 1;
        const bool have_dst = s.dst_buf_before.size() == size_t(len);
        uint64_t d_bad = 0, as_buf_before = 0, on_invalid = 0;
        uint32_t runs = 0, runs_page_start = 0, runs_page_end = 0;
        std::string run_list;
        uint32_t first_run = UINT32_MAX;
        bool in_run = false;
        uint32_t run_start = 0, run_last = 0;
        auto close_run = [&]() {
          ++runs;
          const uint32_t run_end = run_last + 4;
          runs_page_start += ((s.start + run_start) & page_mask) == 0;
          runs_page_end += ((s.start + run_end) & page_mask) == 0;
          if (runs <= 6) {
            run_list += fmt::format(" {:X}+{}", run_start, run_end - run_start);
          }
          in_run = false;
        };
        for (uint32_t i = 0; i + 4 <= len; i += 4) {
          if (!s.written[i]) {
            continue;
          }
          if (!std::memcmp(gpu + s.start + i, s.expected.data() + i, 4)) {
            if (in_run) {
              close_run();
            }
            continue;
          }
          ++d_bad;
          as_buf_before += have_dst && !std::memcmp(gpu + s.start + i,
                                                    s.dst_buf_before.data() + i,
                                                    4);
          const uint32_t pi =
              ((s.start + i) >> psl) - (s.start >> psl);
          on_invalid += pi < s.dst_page_invalid.size() && s.dst_page_invalid[pi];
          if (!in_run) {
            in_run = true;
            run_start = i;
            if (first_run == UINT32_MAX) {
              first_run = i;
            }
          }
          run_last = i;
        }
        if (in_run) {
          close_run();
        }
        // Where else do the first run's 16 bytes exist?
        auto find16 = [&](const uint8_t* hay, size_t hay_len) -> int64_t {
          if (first_run == UINT32_MAX || first_run + 16 > len ||
              hay_len < 16) {
            return -1;
          }
          const uint8_t* needle = gpu + s.start + first_run;
          for (size_t o = 0; o + 16 <= hay_len; o += 4) {
            if (!std::memcmp(hay + o, needle, 16)) {
              return int64_t(o);
            }
          }
          return -1;
        };
        const int64_t in_expected = find16(s.expected.data(), s.expected.size());
        const int64_t in_source = find16(s.src_snap.data(), s.src_snap.size());
        const int64_t in_before = find16(s.before.data(), s.before.size());
        const int64_t in_buf_before =
            find16(s.dst_buf_before.data(), s.dst_buf_before.size());
        uint32_t now_staged = 0, now_precommit = 0, now_gpu_write = 0,
                 now_invalid = 0;
        for (uint32_t page = s.start >> psl; page <= (s.end - 1) >> psl;
             ++page) {
          now_staged += page_last_staged_copy_submission_[page] >= s.submission;
          now_precommit +=
              page_last_precommit_copy_submission_[page] >= s.submission;
          now_gpu_write += page_last_gpu_write_submission_[page] > s.submission;
          now_invalid += !IsRangeValid(page << psl, 1u << psl);
        }
        uint32_t w[4][4] = {};
        if (first_run != UINT32_MAX && first_run + 16 <= len) {
          std::memcpy(w[0], gpu + s.start + first_run, 16);
          std::memcpy(w[1], s.expected.data() + first_run, 16);
          std::memcpy(w[2], s.before.data() + first_run, 16);
          if (have_dst) {
            std::memcpy(w[3], s.dst_buf_before.data() + first_run, 16);
          }
        }
        ++shadow_dst_copies_;
        shadow_dst_bad_words_ += d_bad;
        shadow_dst_as_buf_before_ += as_buf_before;
        shadow_dst_on_invalid_page_ += on_invalid;
        shadow_dst_runs_ += runs;
        shadow_dst_runs_page_start_ += runs_page_start;
        shadow_dst_found_expected_ += in_expected >= 0;
        shadow_dst_found_source_ += in_expected < 0 && in_source >= 0;
        shadow_dst_found_nowhere_ += in_expected < 0 && in_source < 0 &&
                                     in_before < 0 && in_buf_before < 0;
        if (shadow_dst_logged_ < 160) {
          ++shadow_dst_logged_;
          REXLOG_WARN(
              "[memexport-shadow-dst] copy to {:08X}+{} from {:08X}+{} "
              "(submission {}): bad words {} in {} runs ({} start on a page "
              "boundary, {} end on one), first runs (offset+bytes){}; bad "
              "words equal to the buffer before the draw {} (snapshot {}); "
              "bad words on pages not valid at the draw {}; destination "
              "pages at the draw {}: not valid {}, GPU write outstanding {}, "
              "staged upload outstanding {}, precommit upload outstanding "
              "{}, readbacks pending over it {}; pages now: staged upload "
              "since the draw {}, precommit upload since the draw {}, later "
              "GPU write {}, not valid {}; first run's 16 bytes at offset "
              "{:X} found in the expected image at {:X}, the source at {:X}, "
              "guest before at {:X}, buffer before at {:X} (FFFFFFFF = not "
              "found); buffer {:08X} {:08X} {:08X} {:08X} expected {:08X} "
              "{:08X} {:08X} {:08X} guest before {:08X} {:08X} {:08X} {:08X} "
              "buffer before {:08X} {:08X} {:08X} {:08X}",
              s.start, len, s.src_start, s.src_end - s.src_start,
              s.submission, d_bad, runs, runs_page_start, runs_page_end,
              run_list, as_buf_before, have_dst, on_invalid, s.dst_pages,
              s.dst_pages_invalid, s.dst_pages_gpu_write, s.dst_pages_staged,
              s.dst_pages_precommit, s.dst_readbacks_pending, now_staged,
              now_precommit, now_gpu_write, now_invalid, first_run,
              uint32_t(in_expected), uint32_t(in_source), uint32_t(in_before),
              uint32_t(in_buf_before), w[0][0], w[0][1], w[0][2], w[0][3],
              w[1][0], w[1][1], w[1][2], w[1][3], w[2][0], w[2][1], w[2][2],
              w[2][3], w[3][0], w[3][1], w[3][2], w[3][3]);
        }
        if ((shadow_dst_copies_ & 7) == 1) {
          REXLOG_INFO(
              "[memexport-shadow-dst] summary at submission {}: unexplained "
              "bad copies {} (bad words {}); bad words equal to the buffer "
              "before the draw {}; on pages not valid at the draw {}; runs "
              "{}, starting on a page boundary {}; first run found in the "
              "expected image {}, else in the source {}, nowhere {}",
              completed_submission, shadow_dst_copies_, shadow_dst_bad_words_,
              shadow_dst_as_buf_before_, shadow_dst_on_invalid_page_,
              shadow_dst_runs_, shadow_dst_runs_page_start_,
              shadow_dst_found_expected_, shadow_dst_found_source_,
              shadow_dst_found_nowhere_);
        }
      }
      if (!s.src_pending) {
        if (!have_src) {
          ++shadow_bad_src_unknown_;
        } else if (src_guest_changed) {
          ++shadow_bad_src_guest_changed_;
        } else if (src_buffer_differs) {
          ++shadow_bad_src_buffer_only_;
        } else {
          ++shadow_bad_src_same_;
        }
        shadow_bad_words_as_buffer_ += as_buffer;
        shadow_bad_words_as_guest_ += as_guest;
        shadow_bad_words_as_neither_ += as_neither;
        if (s.src_pages_invalid || s.src_pages_gpu_write ||
            s.src_pages_staged || s.src_pages_precommit ||
            s.src_pages_standalone) {
          ++shadow_bad_src_not_settled_;
        }
        if (shadow_src_logged_ < 200) {
          ++shadow_src_logged_;
          REXLOG_WARN(
              "[memexport-shadow-src] copy to {:08X}+{} from {:08X}+{} "
              "(submission {}): source words {}, changed in guest memory "
              "since the draw {}, differing in the buffer from the draw-"
              "time guest bytes {}; bad words equal to the copy of the "
              "buffer's source now {}, of the guest's source now {}, "
              "neither {}; source pages at the draw {}: not valid {}, GPU "
              "write outstanding {}, staged upload outstanding {}, "
              "precommit upload outstanding {}, standalone access {}; "
              "readbacks pending over the source {}",
              s.start, len, s.src_start, s.src_end - s.src_start,
              s.submission, s.src_snap.size() / 4, src_guest_changed,
              src_buffer_differs, as_buffer, as_guest, as_neither,
              s.src_pages, s.src_pages_invalid, s.src_pages_gpu_write,
              s.src_pages_staged, s.src_pages_precommit,
              s.src_pages_standalone, s.src_readbacks_pending);
        }
      }
    }
    // Every copy: which written words did an earlier checked copy also write,
    // and does the buffer hold the earlier copy's value where this one is bad.
    {
      std::vector<const MemexportShadow*> prev;
      for (auto it = shadow_history_.rbegin(); it != shadow_history_.rend();
           ++it) {
        if (it->start < s.end && s.start < it->end) {
          prev.push_back(&*it);  // newest first
        }
      }
      uint64_t shared = 0, shared_diff = 0, bad = 0, as_earlier = 0,
               earlier_other = 0, no_earlier = 0;
      const MemexportShadow* first_prev = nullptr;
      if (!later_export) {
        for (uint32_t i = 0; i + 4 <= len; i += 4) {
          if (!s.written[i]) {
            continue;
          }
          const uint32_t a = s.start + i;
          const MemexportShadow* h = nullptr;
          for (const MemexportShadow* p : prev) {
            if (a >= p->start && a + 4 <= p->end && p->written[a - p->start]) {
              h = p;
              break;
            }
          }
          const bool is_bad =
              std::memcmp(gpu + a, s.expected.data() + i, 4) != 0;
          if (h) {
            ++shared;
            shared_diff += std::memcmp(h->expected.data() + (a - h->start),
                                       s.expected.data() + i, 4) != 0;
            if (!first_prev) {
              first_prev = h;
            }
          }
          if (is_bad) {
            ++bad;
            if (!h) {
              ++no_earlier;
            } else if (!std::memcmp(gpu + a,
                                    h->expected.data() + (a - h->start), 4)) {
              ++as_earlier;
            } else {
              ++earlier_other;
            }
          }
        }
      }
      if (shared) {
        ++shadow_prev_copies_;
        shadow_prev_copies_same_sub_ +=
            first_prev && first_prev->submission == s.submission;
        shadow_prev_copies_bad_ += bad != 0;
        shadow_prev_shared_words_ += shared;
        shadow_prev_shared_diff_words_ += shared_diff;
      }
      shadow_prev_bad_words_ += bad;
      shadow_prev_bad_as_earlier_ += as_earlier;
      shadow_prev_bad_earlier_other_ += earlier_other;
      shadow_prev_bad_no_earlier_ += no_earlier;
      if (!s.src_pending) {
        shadow_prev_np_bad_words_ += bad;
        shadow_prev_np_bad_as_earlier_ += as_earlier;
        shadow_prev_np_bad_earlier_other_ += earlier_other;
        shadow_prev_np_bad_no_earlier_ += no_earlier;
      }
      if (bad && !s.src_pending && shadow_prev_logged_ < 160) {
        ++shadow_prev_logged_;
        REXLOG_WARN(
            "[memexport-shadow-prev] copy to {:08X}+{} from {:08X} "
            "(submission {}, draw {}): bad words {}, of them holding the "
            "value an earlier copy wrote to the same word {}, an earlier "
            "copy wrote the word but another value {}, no earlier copy "
            "wrote the word {}; words this copy shares with an earlier copy "
            "{} (the two write different values to {}); newest earlier copy "
            "to {:08X}+{} from {:08X} (submission {}, draw {}); earlier "
            "copies overlapping the span {}",
            s.start, len, s.src_start, s.submission, s.seq_first, bad,
            as_earlier, earlier_other, no_earlier, shared, shared_diff,
            first_prev ? first_prev->start : 0,
            first_prev ? first_prev->end - first_prev->start : 0,
            first_prev ? first_prev->src_start : 0,
            first_prev ? first_prev->submission : 0,
            first_prev ? first_prev->seq_first : 0, prev.size());
      }
      if (bad && (shadow_prev_bad_words_ - bad) / 20000 !=
                     shadow_prev_bad_words_ / 20000) {
        REXLOG_INFO(
            "[memexport-shadow-prev] summary at submission {}: copies "
            "sharing a written word with an earlier copy {} (earlier copy in "
            "the same submission {}, with a bad word {}); shared words {}, "
            "written with different values {}; bad words in all copies {}: "
            "holding the earlier copy's value {}, earlier copy wrote another "
            "value {}, no earlier copy wrote the word {}; bad words in "
            "copies whose source was not pending {}: holding the earlier "
            "copy's value {}, earlier other {}, no earlier {}; history {}",
            completed_submission, shadow_prev_copies_,
            shadow_prev_copies_same_sub_, shadow_prev_copies_bad_,
            shadow_prev_shared_words_, shadow_prev_shared_diff_words_,
            shadow_prev_bad_words_, shadow_prev_bad_as_earlier_,
            shadow_prev_bad_earlier_other_, shadow_prev_bad_no_earlier_,
            shadow_prev_np_bad_words_, shadow_prev_np_bad_as_earlier_,
            shadow_prev_np_bad_earlier_other_, shadow_prev_np_bad_no_earlier_,
            shadow_history_.size());
      }
    }
    {
      MemexportShadow keep = std::move(memexport_shadows_.front());
      memexport_shadows_.pop_front();
      keep.rerun = nullptr;
      keep.before = {};
      keep.src_snap = {};
      keep.dst_buf_before = {};
      keep.losers = {};
      shadow_history_bytes_ += keep.expected.size();
      shadow_history_.push_back(std::move(keep));
      while (shadow_history_.size() > 400 ||
             shadow_history_bytes_ > (192ull << 20)) {
        shadow_history_bytes_ -= shadow_history_.front().expected.size();
        shadow_history_.pop_front();
      }
    }
  }
}

void MetalSharedMemory::ApplyCompletedMemexportReadbacks(
    uint64_t completed_submission) {
  CheckCompletedMemexportShadows(completed_submission);
  if (pending_readbacks_.empty() ||
      pending_readbacks_.front().submission > completed_submission || !buffer_) {
    return;
  }
  // Pairs with the release in the command buffer completion handler; shared
  // storage on Apple Silicon is coherent once the command buffer completed.
  std::atomic_thread_fence(std::memory_order_acquire);
  uint8_t* guest = static_cast<uint8_t*>(memory().TranslatePhysical(0));
  const uint8_t* gpu = static_cast<const uint8_t*>(buffer_->contents());
  if (!guest || !gpu) {
    return;
  }
  const uint64_t t0 = PreserveNowUs();
  // Entries are queued in submission order, so the completed ones are a prefix.
  size_t done = 0;
  while (done < pending_readbacks_.size() &&
         pending_readbacks_[done].submission <= completed_submission) {
    ++done;
  }
  uint64_t bytes = 0;
  uint64_t ranges = 0;
  uint64_t cpu_skip_bytes = 0;
  std::vector<size_t> rebase_pending;
  std::vector<size_t> rebase_preserved;
  for (size_t i = 0; i < done; ++i) {
    const PendingReadback& e = pending_readbacks_[i];
    const uint32_t len = e.end - e.start;
    // A later completed export of the same range copies newer data anyway.
    bool superseded = false;
    for (size_t j = i + 1; j < done; ++j) {
      if (pending_readbacks_[j].start == e.start &&
          pending_readbacks_[j].end == e.end) {
        superseded = true;
        break;
      }
    }
    if (superseded) {
      continue;
    }
    if (e.hole) {
      ++hole_readbacks_;
      if (e.before.size() == len &&
          std::memcmp(gpu + e.start, e.before.data(), len) == 0) {
        ++hole_readbacks_wiped_;
      }
    }
    const uint64_t guest_hash_now = XXH3_64bits(guest + e.start, len);
    const bool cpu_changed = guest_hash_now != e.guest_hash;
    bool merge = false;
    if (cpu_changed) {
      // The CPU wrote into the export range after the export draw.
      ++merge_ranges_;
      uint32_t words_cpu = 0, words_gpu = 0, words_both = 0;
      const bool snapshot = e.before.size() == len;
      if (snapshot) {
        const uint8_t* before = e.before.data();
        for (uint32_t o = 0; o + 4 <= len; o += 4) {
          const bool c = std::memcmp(guest + e.start + o, before + o, 4) != 0;
          const bool g = std::memcmp(gpu + e.start + o, before + o, 4) != 0;
          words_cpu += c && !g;
          words_gpu += g && !c;
          words_both += c && g;
        }
        merge_words_cpu_ += words_cpu;
        merge_words_gpu_ += words_gpu;
        merge_words_both_ += words_both;
      }
      merge = snapshot && MemexportMergeEnabled();
      if (!merge) {
        ++merge_ranges_lost_;
      }
      if (merge_logged_ < 32) {
        ++merge_logged_;
        REXLOG_WARN(
            "[memexport-merge] #{}: range {:08X}+{} export_sub={} completed={}"
            ": guest bytes changed since the export draw; words changed by "
            "CPU only {}, by the export only {}, by both {} ({})",
            merge_logged_, e.start, len, e.submission, completed_submission,
            words_cpu, words_gpu, words_both,
            merge ? "export words copied back"
                  : "whole export dropped");
      }
      if (!merge) {
        // Old behaviour: the CPU data wins for the whole range (that write
        // also invalidated the page, so it gets re-uploaded).
        cpu_skip_bytes += len;
        continue;
      }
    }
    // Hashes taken by later exports / fix A that still describe the guest
    // bytes must follow this (non-CPU) write, or it would look like a CPU
    // write to them.
    rebase_pending.clear();
    rebase_preserved.clear();
    for (size_t j = i + 1; j < pending_readbacks_.size(); ++j) {
      const PendingReadback& l = pending_readbacks_[j];
      if (l.end <= e.start || l.start >= e.end) continue;
      const uint64_t h = (l.start == e.start && l.end == e.end)
                             ? guest_hash_now
                             : XXH3_64bits(guest + l.start, l.end - l.start);
      if (h == l.guest_hash) rebase_pending.push_back(j);
    }
    for (size_t j = 0; j < preserved_exports_.size(); ++j) {
      const PreservedExport& p = preserved_exports_[j];
      if (p.end <= e.start || p.start >= e.end) continue;
      const uint64_t h = (p.start == e.start && p.end == e.end)
                             ? guest_hash_now
                             : XXH3_64bits(guest + p.start, p.end - p.start);
      if (h == p.guest_hash) rebase_preserved.push_back(j);
    }
    {
      // Diagnostic: where the readback changes guest memory, per 16 MiB region.
      uint32_t changed_chunks = 0;
      for (uint32_t p = e.start; p < e.end;) {
        const uint32_t chunk_end = std::min(e.end, (p | 0xFFFu) + 1);
        if (std::memcmp(guest + p, gpu + p, chunk_end - p)) ++changed_chunks;
        p = chunk_end;
      }
      ReadbackRegionStats& rs = readback_regions_[e.start >> 24];
      ++rs.ranges;
      rs.bytes += len;
      rs.changed_chunks += changed_chunks;
      if ((e.start >> 24) != 0x06 && readback_region_logged_ < 48) {
        ++readback_region_logged_;
        REXLOG_WARN(
            "[memexport-readback] region #{}: range {:08X}+{} export_sub={} "
            "completed={} export_frame={} frame={}: {} of {} 4K chunks differ "
            "from guest memory",
            readback_region_logged_, e.start, len, e.submission,
            completed_submission, e.export_frame, frame_log::CurrentFrame(),
            changed_chunks, (len + 0xFFF) >> 12);
      }
    }
    // Later exports' snapshots of these bytes must follow this write too.
    std::vector<size_t> snap_later;
    for (size_t j = i + 1; j < pending_readbacks_.size(); ++j) {
      const PendingReadback& l = pending_readbacks_[j];
      if (l.end <= e.start || l.start >= e.end || l.before.empty()) continue;
      snap_later.push_back(j);
    }
    if (merge) {
      const uint8_t* before = e.before.data();
      for (uint32_t o = 0; o + 4 <= len; o += 4) {
        const uint32_t p = e.start + o;
        if (std::memcmp(gpu + p, before + o, 4) == 0 ||
            std::memcmp(guest + p, before + o, 4) != 0) {
          continue;
        }
        std::memcpy(guest + p, gpu + p, 4);
        ++merge_words_applied_;
        for (size_t j : snap_later) {
          PendingReadback& l = pending_readbacks_[j];
          if (p >= l.start && p + 4 <= l.end) {
            std::memcpy(l.before.data() + (p - l.start), gpu + p, 4);
          }
        }
      }
    } else {
      // A page that is invalid now was written by the CPU after the export
      // (possibly with the same bytes the hash already saw, when a file read
      // reports its pages only after writing them): the guest data wins.
      const uint32_t page = uint32_t(1) << page_size_log2();
      const bool guard = MemexportGuardEnabled();
      uint32_t invalid_pages = 0, stale_pages = 0;
      for (uint32_t p = e.start; p < e.end;) {
        const uint32_t page_end = std::min(e.end, (p | (page - 1)) + 1);
        const uint32_t n = page_end - p;
        if (IsRangeValid(p & ~(page - 1), page)) {
          std::memcpy(guest + p, gpu + p, n);
        } else {
          ++invalid_pages;
          if (std::memcmp(guest + p, gpu + p, n)) {
            ++stale_pages;
          }
          if (!guard) {
            std::memcpy(guest + p, gpu + p, n);
          }
        }
        p = page_end;
      }
      for (size_t j : snap_later) {
        PendingReadback& l = pending_readbacks_[j];
        const uint32_t os = std::max(l.start, e.start);
        const uint32_t oe = std::min(l.end, e.end);
        std::memcpy(l.before.data() + (os - l.start), guest + os, oe - os);
      }
      if (invalid_pages) {
        ++guard_readback_ranges_hit_;
        guard_readback_pages_invalid_ += invalid_pages;
        guard_readback_pages_stale_ += stale_pages;
        if (stale_pages && guard_readback_ranges_hit_ <= 16) {
          REXLOG_WARN(
              "[memexport-guard] readback #{}: range {:08X}+{} has {} pages "
              "invalid, {} of them differ from guest memory ({})",
              guard_readback_ranges_hit_, e.start, len, invalid_pages,
              stale_pages, guard ? "left alone" : "overwritten");
        }
      }
    }
    const uint64_t new_hash = XXH3_64bits(guest + e.start, len);
    for (size_t j : rebase_pending) {
      PendingReadback& l = pending_readbacks_[j];
      l.guest_hash = (l.start == e.start && l.end == e.end)
                         ? new_hash
                         : XXH3_64bits(guest + l.start, l.end - l.start);
    }
    for (size_t j : rebase_preserved) {
      PreservedExport& p = preserved_exports_[j];
      p.guest_hash = (p.start == e.start && p.end == e.end)
                         ? new_hash
                         : XXH3_64bits(guest + p.start, p.end - p.start);
    }
    bytes += len;
    ++ranges;
    static uint32_t logged = 0;
    if (logged < 24) {
      ++logged;
      REXLOG_INFO(
          "[memexport-readback] applied 0x{:08X} len={} export_sub={} "
          "completed={} export_frame={} frame={} changed={}",
          e.start, len, e.submission, completed_submission, e.export_frame,
          frame_log::CurrentFrame(), new_hash != guest_hash_now ? 1 : 0);
    }
  }
  pending_readbacks_.erase(pending_readbacks_.begin(),
                           pending_readbacks_.begin() + done);
  if (completed_submission / 600 != guard_last_summary_) {
    guard_last_summary_ = completed_submission / 600;
    std::string regions;
    for (const auto& [region, rs] : readback_regions_) {
      regions += fmt::format(" {:02X}:{}r/{}b/{}c", region, rs.ranges, rs.bytes,
                             rs.changed_chunks);
    }
    REXLOG_INFO("[memexport-readback] regions at submission {}:{}",
                completed_submission, regions);
    REXLOG_INFO(
        "[memexport-guard] summary at submission {}: draw ranges {}, with "
        "CPU-written pages {} ({} pages); readback ranges with invalid pages "
        "{} ({} pages, {} differing from guest memory)",
        completed_submission, guard_draw_ranges_, guard_draw_ranges_hit_,
        guard_draw_pages_invalid_, guard_readback_ranges_hit_,
        guard_readback_pages_invalid_, guard_readback_pages_stale_);
    REXLOG_INFO(
        "[memexport-merge] summary at submission {}: readbacks whose guest "
        "bytes changed {} (dropped whole {}); words changed by CPU only {}, "
        "by the export only {}, by both {}; export words copied back {}; "
        "re-uploads over a changed export range {} (readback still pending "
        "{}, export words lost {})",
        completed_submission, merge_ranges_, merge_ranges_lost_,
        merge_words_cpu_, merge_words_gpu_, merge_words_both_,
        merge_words_applied_, preserve_dropped_, preserve_dropped_pending_,
        preserve_dropped_gpu_words_);
    REXLOG_INFO(
        "[memexport-hole] summary at submission {}: uploads over a pending "
        "export with no protection {} ({} bytes; guest unchanged since the "
        "export {}, export older than the preserve lifetime {}); bytes kept "
        "out of the upload {}; readbacks landing after one {} (buffer back "
        "to pre-export bytes {})",
        completed_submission, hole_events_, hole_bytes_,
        hole_events_guest_same_, hole_events_expired_, hole_kept_bytes_,
        hole_readbacks_, hole_readbacks_wiped_);
    if (shadow_draws_ || shadow_dropped_) {
      REXLOG_INFO(
          "[memexport-shadow] summary at submission {}: copies checked {} "
          "(with a missing or wrong word {}); words ok {}, missing {}, wrong "
          "{} (missing or wrong with the source pending or not valid {}), "
          "later CPU write {}, under a later export {}; copies whose source "
          "was pending {}, not valid {}, with an upload over them {}; not "
          "checked {}",
          completed_submission, shadow_draws_, shadow_draws_bad_,
          shadow_words_ok_, shadow_words_missing_, shadow_words_wrong_,
          shadow_words_wrong_src_, shadow_words_cpu_later_,
          shadow_words_later_export_, shadow_draws_src_pending_,
          shadow_draws_src_invalid_, shadow_draws_hole_, shadow_dropped_);
      REXLOG_INFO(
          "[shmem-diverge] summary at submission {}: copy sources checked {} "
          "({} settled bytes); valid, settled pages where the buffer differs "
          "from guest memory {} ({} words; zero in guest {}, zero in the "
          "buffer {}); bad copies: source not pending {}, source diverged "
          "{}, neither {} ({} words)",
          completed_submission, src_div_checks_, src_div_settled_bytes_,
          src_div_pages_, src_div_words_, src_div_words_guest_zero_,
          src_div_words_buffer_zero_, shadow_draws_bad_nopend_,
          shadow_draws_bad_div_, shadow_draws_bad_unexplained_,
          shadow_words_bad_unexplained_);
      REXLOG_INFO(
          "[memexport-shadow-dup] summary at submission {}: copies that "
          "write a word twice with different values {} (words written "
          "more than once {}, with different values {}); bad words in "
          "all copies {}, of them equal to a value an earlier vertex of "
          "the same draw wrote there {} (in {} copies); bad words in "
          "copies whose source was not pending {}, of them such {}",
          completed_submission, shadow_dup_copies_,
          shadow_dup_rewritten_words_, shadow_dup_contested_words_,
          shadow_bad_words_all_, shadow_dup_bad_words_,
          shadow_dup_bad_copies_, shadow_bad_words_not_pending_,
          shadow_dup_bad_words_not_pending_);
      REXLOG_INFO(
          "[memexport-shadow-src] summary at submission {}: bad copies "
          "with the source not pending, by what the source did after the "
          "draw: guest memory changed {}, only the buffer differs {}, "
          "source unchanged {}, no snapshot {}; of them with a source "
          "page not settled at the draw {}; bad words equal to the copy "
          "of the buffer's source now {}, of the guest's source now {}, "
          "neither {}",
          completed_submission, shadow_bad_src_guest_changed_,
          shadow_bad_src_buffer_only_, shadow_bad_src_same_,
          shadow_bad_src_unknown_, shadow_bad_src_not_settled_,
          shadow_bad_words_as_buffer_, shadow_bad_words_as_guest_,
          shadow_bad_words_as_neither_);
    }
  }
  if (PreserveTraceEnabled()) {
    const uint64_t frame = frame_log::CurrentFrame();
    if (bytes) {
      frame_log::Record("tr_memexport_readback_bytes", frame, bytes);
      frame_log::Record("tr_memexport_readback_ranges", frame, ranges);
      frame_log::Record("tr_memexport_readback_us", frame,
                        PreserveNowUs() - t0);
    }
    if (cpu_skip_bytes) {
      frame_log::Record("tr_memexport_readback_cpu_skip_bytes", frame,
                        cpu_skip_bytes);
    }
  }
}

bool MetalSharedMemory::UploadVerifyEnabled() {
  static const bool enabled = [] {
    const char* v = MetalDebugEnv("UPLOAD_VERIFY");
    const bool on = v && v[0] == '1';
    if (on) {
      REXLOG_INFO("[upload-verify] on (trace-only): staged copies are checked "
                  "against the GPU buffer after their submission completes");
    }
    return on;
  }();
  return enabled;
}

void MetalSharedMemory::NoteUploadVerify(uint32_t start, uint32_t size,
                                         bool precommit) {
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  if (!guest || !size || pending_verifies_.size() >= 65536) {
    return;
  }
  pending_verifies_.push_back({start, size, XXH3_64bits(guest + start, size),
                               command_processor_.GetCurrentSubmission(),
                               precommit});
}

void MetalSharedMemory::VerifyCompletedUploads(uint64_t completed_submission) {
  if (pending_verifies_.empty() || !buffer_) {
    return;
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint8_t* guest =
      static_cast<const uint8_t*>(memory().TranslatePhysical(0));
  const uint8_t* gpu = static_cast<const uint8_t*>(buffer_->contents());
  if (!guest || !gpu) {
    return;
  }
  // [route][0 ok, 1 mismatch, 2 guest changed since the copy]
  static uint64_t counts[2][3] = {};
  static uint64_t checked = 0;
  while (!pending_verifies_.empty() &&
         pending_verifies_.front().submission <= completed_submission) {
    const PendingVerify& v = pending_verifies_.front();
    const int route = v.precommit ? 1 : 0;
    if (XXH3_64bits(guest + v.start, v.size) != v.hash) {
      ++counts[route][2];
    } else if (XXH3_64bits(gpu + v.start, v.size) == v.hash) {
      ++counts[route][0];
    } else {
      ++counts[route][1];
    }
    pending_verifies_.pop_front();
    ++checked;
    if ((checked & (checked - 1)) == 0 || checked % 20000 == 0) {
      REXLOG_INFO(
          "[upload-verify] in-pass ok {} mismatch {} guest-changed {} | "
          "pre-commit ok {} mismatch {} guest-changed {}",
          counts[0][0], counts[0][1], counts[0][2], counts[1][0], counts[1][1],
          counts[1][2]);
    }
  }
}

MetalSharedMemory::MetalSharedMemory(MetalCommandProcessor& command_processor,
                                     rex::memory::Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

MetalSharedMemory::~MetalSharedMemory() { Shutdown(); }

bool MetalSharedMemory::Initialize() {
  if (!InitializeCommon()) {
    return false;
  }

  const ui::metal::MetalProvider& provider =
      command_processor_.GetMetalProvider();
  MTL::Device* device = provider.GetDevice();

  if (!device) {
    REXLOG_ERROR("Metal device is null in MetalSharedMemory::Initialize");
    return false;
  }

  // Create Metal buffer - similar to D3D12's approach
  // On Apple Silicon, ResourceStorageModeShared gives CPU/GPU access
  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    REXLOG_ERROR("Metal shared memory: Xbox RAM is null");
    return false;
  }

  buffer_ = device->newBuffer(kBufferSize, MTL::ResourceStorageModeShared);
  if (!buffer_) {
    REXLOG_ERROR("Failed to create Metal shared memory buffer");
    return false;
  }

  upload_buffer_pool_ = std::make_unique<MetalUploadBufferPool>(
      device, rex::align(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                        size_t(1) << page_size_log2()));
  marked_gpu_access_cache_.fill(MarkedGpuAccessRange());
  dedupe_gpu_access_ = REXCVAR_GET(metal_dedupe_gpu_access);
  page_last_main_gpu_access_submission_.assign(kBufferSize >> page_size_log2(),
                                               0);
  page_standalone_gpu_access_counts_.assign(kBufferSize >> page_size_log2(), 0);
  page_last_gpu_write_submission_.assign(kBufferSize >> page_size_log2(), 0);
  page_last_staged_copy_submission_.assign(kBufferSize >> page_size_log2(), 0);
  page_last_precommit_copy_submission_.assign(kBufferSize >> page_size_log2(),
                                              0);
  (void)MemexportPreserveEnabled();  // logs once at startup when enabled
  (void)MemexportReadbackEnabled();  // logs once at startup when enabled

  return true;
}

void MetalSharedMemory::ClearCache() {
  SharedMemory::ClearCache();
  pending_readbacks_.clear();

  if (upload_buffer_pool_) {
    upload_buffer_pool_->ClearCache();
  }
}

void MetalSharedMemory::MarkGpuAccess(uint32_t start, uint32_t length,
                                      uint64_t submission_index) {
  if (!length || !submission_index ||
      page_last_main_gpu_access_submission_.empty()) {
    return;
  }
  if (start >= kBufferSize) {
    return;
  }
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  if (end <= start) {
    return;
  }
  MarkedGpuAccessRange& marked =
      marked_gpu_access_cache_[((start >> 8) ^ (length * 0x9E3779B1u)) &
                               (kMarkedGpuAccessCacheSize - 1)];
  if (dedupe_gpu_access_ && marked.start == start && marked.length == length &&
      marked.submission >= submission_index) {
    return;
  }
  marked.start = start;
  marked.length = length;
  marked.submission = submission_index;
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  page_last = std::min<uint32_t>(
      page_last,
      static_cast<uint32_t>(page_last_main_gpu_access_submission_.size() - 1));
  for (uint32_t page = page_first; page <= page_last; ++page) {
    page_last_main_gpu_access_submission_[page] =
        std::max(page_last_main_gpu_access_submission_[page], submission_index);
  }
}

void MetalSharedMemory::TrackStandaloneGpuAccess(
    MTL::CommandBuffer* command_buffer,
    const std::pair<uint32_t, uint32_t>* ranges, uint32_t range_count) {
  if (!command_buffer || !ranges || !range_count ||
      page_standalone_gpu_access_counts_.empty()) {
    return;
  }
  auto tracked_page_ranges =
      std::make_shared<std::vector<std::pair<uint32_t, uint32_t>>>();
  tracked_page_ranges->reserve(range_count);
  for (uint32_t i = 0; i < range_count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length || start >= kBufferSize) {
      continue;
    }
    uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
    if (end <= start) {
      continue;
    }
    uint32_t page_first = start >> page_size_log2();
    uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
    page_last = std::min<uint32_t>(
        page_last,
        static_cast<uint32_t>(page_standalone_gpu_access_counts_.size() - 1));
    tracked_page_ranges->push_back({page_first, page_last});
  }
  if (tracked_page_ranges->empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
    for (const auto& page_range : *tracked_page_ranges) {
      for (uint32_t page = page_range.first; page <= page_range.second;
           ++page) {
        ++page_standalone_gpu_access_counts_[page];
      }
    }
  }
  MetalSharedMemory* shared_memory = this;
  command_buffer->addCompletedHandler(^(MTL::CommandBuffer*) {
    std::lock_guard<std::mutex> lock(
        shared_memory->standalone_gpu_access_mutex_);
    for (const auto& page_range : *tracked_page_ranges) {
      for (uint32_t page = page_range.first; page <= page_range.second;
           ++page) {
        if (page < shared_memory->page_standalone_gpu_access_counts_.size() &&
            shared_memory->page_standalone_gpu_access_counts_[page]) {
          --shared_memory->page_standalone_gpu_access_counts_[page];
        }
      }
    }
  });
}

void MetalSharedMemory::MarkGpuWrite(uint32_t start, uint32_t length,
                                     uint64_t submission_index,
                                     bool staged_copy) {
  if (!length || !submission_index || start >= kBufferSize ||
      page_last_gpu_write_submission_.empty()) {
    return;
  }
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  std::vector<uint64_t>& pages = staged_copy
                                    ? page_last_staged_copy_submission_
                                    : page_last_gpu_write_submission_;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    pages[page] = std::max(pages[page], submission_index);
  }
}

void MetalSharedMemory::MarkPrecommitCopy(uint32_t start, uint32_t length,
                                          uint64_t submission_index) {
  if (!length || start >= kBufferSize ||
      page_last_precommit_copy_submission_.empty()) {
    return;
  }
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  for (uint32_t page = start >> page_size_log2();
       page <= static_cast<uint32_t>((end - 1) >> page_size_log2()); ++page) {
    page_last_precommit_copy_submission_[page] = submission_index;
  }
}

uint32_t MetalSharedMemory::RangeOpenWriteReasons(uint32_t start,
                                                  uint32_t length) {
  if (!length || start >= kBufferSize ||
      page_last_gpu_write_submission_.empty()) {
    return 0;
  }
  uint32_t reasons = 0;
  const uint64_t committed = command_processor_.GetLastCommittedSubmission();
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (page_last_gpu_write_submission_[page] > committed) reasons |= 1;
    if (page_last_staged_copy_submission_[page] > committed) reasons |= 2;
    if (page_standalone_gpu_access_counts_[page]) reasons |= 4;
    if (page_last_precommit_copy_submission_[page] > committed) reasons |= 32;
  }
  return reasons;
}

const uint8_t* MetalSharedMemory::StableContents(uint32_t start,
                                                uint32_t length) {
  if (!length || !buffer_ || uint64_t(start) + length > kBufferSize ||
      page_last_gpu_write_submission_.empty() || !IsRangeValid(start, length)) {
    return nullptr;
  }
  const uint8_t* contents = static_cast<const uint8_t*>(buffer_->contents());
  if (!contents) return nullptr;
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = (start + length - 1) >> page_size_log2();
  std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (page_last_gpu_write_submission_[page] > completed ||
        page_last_staged_copy_submission_[page] > completed ||
        page_last_precommit_copy_submission_[page] > completed ||
        page_standalone_gpu_access_counts_[page]) {
      return nullptr;
    }
  }
  return contents + start;
}

bool MetalSharedMemory::RangeTouchedByOpenOrStandalone(uint32_t start,
                                                       uint32_t length) {
  if (!length || start >= kBufferSize ||
      page_last_main_gpu_access_submission_.empty()) {
    return false;
  }
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  const uint64_t committed = command_processor_.GetLastCommittedSubmission();
  uint64_t end = std::min(uint64_t(start) + length, uint64_t(kBufferSize));
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
  std::lock_guard<std::mutex> lock(standalone_gpu_access_mutex_);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    const PageAccess access =
        ClassifyPageAccessLocked(page, completed, committed, nullptr);
    if (access == PageAccess::kCurrent || access == PageAccess::kStandalone) {
      return true;
    }
  }
  return false;
}

MetalSharedMemory::UploadRouteInfo MetalSharedMemory::GetUploadRouteInfo(
    const SharedMemory::Range* ranges, uint32_t range_count) {
  UploadRouteInfo info;
  if (!ranges || !range_count) {
    return info;
  }

  const uint32_t page_size = 1u << page_size_log2();
  const uint64_t completed_submission =
      command_processor_.GetCompletedSubmission();
  const uint64_t committed_submission =
      command_processor_.GetLastCommittedSubmission();
  const bool direct_write_enabled = REXCVAR_GET(metal_shared_memory_direct_write) &&
                                    buffer_ && buffer_->contents() != nullptr;
  const bool precommit_allowed =
      command_processor_.SharedMemoryPrecommitAllowed();

  std::lock_guard<std::mutex> standalone_lock(standalone_gpu_access_mutex_);
  for (uint32_t i = 0; i < range_count; ++i) {
    const SharedMemory::Range& range = ranges[i];
    if (!range.length || range.start >= kBufferSize) {
      continue;
    }
    uint64_t end =
        std::min(uint64_t(range.start) + range.length, uint64_t(kBufferSize));
    if (end <= range.start) {
      continue;
    }
    uint32_t page_first = range.start >> page_size_log2();
    uint32_t page_last = static_cast<uint32_t>((end - 1) >> page_size_log2());
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t page_start = uint64_t(page) << page_size_log2();
      uint32_t page_length = static_cast<uint32_t>(
          std::min<uint64_t>(page_size, uint64_t(kBufferSize) - page_start));
      if (!page_length ||
          IsRangeValid(static_cast<uint32_t>(page_start), page_length)) {
        continue;
      }

      info.upload_bytes += page_length;
      const PageAccess access = ClassifyPageAccessLocked(
          page, completed_submission, committed_submission, nullptr);
      switch (access) {
        case PageAccess::kCompleted:
          ++info.pages_completed;
          break;
        case PageAccess::kInflight:
          ++info.pages_inflight;
          break;
        case PageAccess::kCurrent:
          ++info.pages_current;
          break;
        case PageAccess::kStandalone:
          ++info.pages_standalone;
          break;
      }
      if (direct_write_enabled && access == PageAccess::kCompleted) {
        info.direct_bytes += page_length;
      } else {
        info.staged_bytes += page_length;
        if (precommit_allowed && access == PageAccess::kInflight) {
          info.precommit_bytes += page_length;
        } else {
          info.inpass_staged_bytes += page_length;
        }
      }
    }
  }
  return info;
}

bool MetalSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  static bool first_upload = true;
  if (first_upload) {
    first_upload = false;
    const uint32_t page_size = 1u << page_size_log2();
    REXLOG_DEBUG("MetalSharedMemory::UploadRanges: page_size={}, {} ranges to upload",
           page_size, num_upload_ranges);
    for (uint32_t i = 0; i < std::min(5u, num_upload_ranges); i++) {
      uint32_t start_byte = upload_page_ranges[i].first * page_size;
      uint32_t length_bytes = upload_page_ranges[i].second * page_size;
      REXLOG_DEBUG("  Range[{}]: page={} count={} -> byte offset=0x{:08X} length={}",
             i, upload_page_ranges[i].first, upload_page_ranges[i].second,
             start_byte, length_bytes);
    }
  }

  if (!buffer_ || num_upload_ranges == 0) {
    return true;
  }

  void* xbox_ram = memory().TranslatePhysical(0);
  if (!xbox_ram) {
    REXLOG_ERROR("MetalSharedMemory::UploadRanges: Xbox RAM is null");
    return false;
  }
  uint8_t* xbox_data = static_cast<uint8_t*>(xbox_ram);

  const uint32_t page_size = 1u << page_size_log2();
  if (!upload_buffer_pool_) {
    REXLOG_ERROR("MetalSharedMemory::UploadRanges: upload buffer pool is null");
    return false;
  }
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  if (MemexportReadbackEnabled()) {
    // Bring guest RAM up to date with completed memexport output first, so
    // the re-upload below writes the GPU's data back rather than stale bytes.
    ApplyCompletedMemexportReadbacks(
        command_processor_.GetCompletedSubmission());
  }

  MTL::BlitCommandEncoder* blit_encoder = nullptr;
  auto get_blit_encoder = [&]() -> MTL::BlitCommandEncoder* {
    if (!blit_encoder) {
      blit_encoder = command_processor_.GetSharedMemoryUploadBlitEncoder();
    }
    return blit_encoder;
  };

  uint32_t merged_start = 0;
  uint32_t merged_end = 0;
  bool have_merged = false;

  auto copy_guest_bytes = [&](uint8_t* dest, uint32_t offset, size_t size) {
    if (size < (1ULL << 32) && size > 8192) {
      memory::vastcpy(dest, xbox_data + offset, static_cast<uint32_t>(size));
      swcache::WriteFence();
    } else {
      std::memcpy(dest, xbox_data + offset, size);
    }
  };

  uint64_t direct_eligible_bytes = 0;
  uint64_t staged_required_bytes = 0;
  uint8_t* shared_buffer_contents = static_cast<uint8_t*>(buffer_->contents());
  const bool has_shared_buffer_contents = shared_buffer_contents != nullptr;
  const uint64_t completed_submission =
      command_processor_.GetCompletedSubmission();
  const uint64_t committed_submission =
      command_processor_.GetLastCommittedSubmission();
  // [upload-precommit] trace: categories of the staged pages seen so far,
  // reported as the cause when the first staged copy ends a render pass.
  bool staged_has_current = false;
  bool staged_has_standalone = false;
  bool staged_has_inflight = false;
  // [upload-precommit] decided once per call; see
  // MetalCommandProcessor::SharedMemoryPrecommitAllowed.
  const bool precommit_allowed =
      has_shared_buffer_contents &&
      command_processor_.SharedMemoryPrecommitAllowed();
  uint32_t precommit_pages = 0;
  bool precommit_fell_back = false;

  auto record_direct_write_reject =
      [&](MetalCommandProcessor::SharedMemoryDirectWriteRejectReason reason,
          uint64_t bytes) {
        command_processor_.RecordSharedMemoryDirectWriteReject(reason, bytes);
      };

  // [upload-precommit] route of a run: direct CPU write, staged blit in the
  // current command buffer, or staged blit in a pre-commit command buffer
  // that first waits for main command buffer `wait` (its pages' last reader).
  enum : uint8_t { kRouteStaged = 0, kRouteDirect = 1, kRoutePrecommit = 2 };
  struct UploadRun {
    uint32_t start;
    uint32_t end;
    bool direct_safe;
    uint8_t route;
    uint64_t wait;
  };

  auto append_upload_run = [](std::vector<UploadRun>& runs, uint32_t start,
                              uint32_t end, bool direct_safe,
                              uint8_t route = 0xFF, uint64_t wait = 0) {
    if (end <= start) {
      return;
    }
    if (route == 0xFF) {
      route = direct_safe ? kRouteDirect : kRouteStaged;
    }
    if (!runs.empty() && runs.back().route == route &&
        runs.back().end == start) {
      runs.back().end = end;
      runs.back().wait = std::max(runs.back().wait, wait);
      return;
    }
    runs.push_back({start, end, direct_safe, route, wait});
  };

  auto build_upload_runs = [&](uint32_t start, uint32_t end,
                               std::vector<UploadRun>& runs) {
    if (end <= start) {
      return;
    }
    if (!has_shared_buffer_contents) {
      uint64_t bytes = uint64_t(end) - start;
      staged_required_bytes += bytes;
      record_direct_write_reject(
          MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
              kNoSharedBufferContents,
          bytes);
      append_upload_run(runs, start, end, false);
      return;
    }
    bool saw_direct = false;
    bool saw_staged = false;
    uint32_t page_first = start >> page_size_log2();
    uint32_t page_last = (end - 1) >> page_size_log2();
    std::lock_guard<std::mutex> standalone_lock(standalone_gpu_access_mutex_);
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t page_start = uint64_t(page) << page_size_log2();
      uint64_t page_end = page_start + page_size;
      uint64_t byte_start = std::max<uint64_t>(page_start, start);
      uint64_t byte_end = std::min<uint64_t>(page_end, end);
      uint64_t bytes = byte_end - byte_start;
      if (!bytes) {
        continue;
      }

      uint64_t last_access = 0;
      const PageAccess access = ClassifyPageAccessLocked(
          page, completed_submission, committed_submission, &last_access);
      UploadTraceCounters& trace = UploadTrace();
      if (access == PageAccess::kStandalone) {
        ++trace.pg_standalone;
        staged_has_standalone = true;
        staged_required_bytes += bytes;
        saw_staged = true;
        append_upload_run(runs, static_cast<uint32_t>(byte_start),
                          static_cast<uint32_t>(byte_end), false);
        record_direct_write_reject(
            MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
                kStandaloneAccessInFlight,
            bytes);
        continue;
      }

      if (access != PageAccess::kCompleted) {
        if (access == PageAccess::kCurrent) {
          ++trace.pg_current;
          staged_has_current = true;
        } else {
          ++trace.pg_inflight;
          if (precommit_allowed) {
            // Read only by committed command buffers: copy in a command
            // buffer committed ahead of the current one, after last_access
            // has finished on the GPU, instead of ending the render pass.
            staged_required_bytes += bytes;
            saw_staged = true;
            append_upload_run(runs, static_cast<uint32_t>(byte_start),
                              static_cast<uint32_t>(byte_end), false,
                              kRoutePrecommit, last_access);
            record_direct_write_reject(
                MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
                    kMainGpuAccessInFlight,
                bytes);
            continue;
          }
          staged_has_inflight = true;
        }
        staged_required_bytes += bytes;
        saw_staged = true;
        append_upload_run(runs, static_cast<uint32_t>(byte_start),
                          static_cast<uint32_t>(byte_end), false);
        record_direct_write_reject(
            MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
                kMainGpuAccessInFlight,
            bytes);
        continue;
      }

      ++trace.pg_completed;
      direct_eligible_bytes += bytes;
      saw_direct = true;
      append_upload_run(runs, static_cast<uint32_t>(byte_start),
                        static_cast<uint32_t>(byte_end), true);
    }

    if (saw_direct && saw_staged) {
      record_direct_write_reject(
          MetalCommandProcessor::SharedMemoryDirectWriteRejectReason::
              kMixedRangeSplit,
          uint64_t(end) - start);
    }
  };

  auto direct_write_run = [&](uint32_t start, uint32_t end) {
    uint32_t size = end - start;
    if (ShmemValidFirstEnabled()) {
      // CR-02 (FABLE2_SHMEM_VALID_FIRST, default on): validate, then copy,
      // like the staged path below and the SDK's D3D12/Vulkan uploaders.
      // MakeRangeValid re-arms the page write watch first, so a guest write
      // that lands during the copy faults, invalidates the page and is
      // re-uploaded instead of being lost behind a "valid" page.
      MakeRangeValid(start, size, false);
      copy_guest_bytes(shared_buffer_contents + start, start, size);
      swcache::WriteFence();
    } else {
      copy_guest_bytes(shared_buffer_contents + start, start, size);
      swcache::WriteFence();
      MakeRangeValid(start, size, false);
    }
    if (PreserveTraceEnabled()) {
      // Trace-only race counter (FABLE2_DRAW_SKIP_TRACE=1): a run whose
      // bytes differ from guest RAM while its pages are still marked valid
      // is a CPU write the page watch missed.
      static std::atomic<uint64_t> runs{0}, stale_valid{0};
      const uint64_t n = runs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (std::memcmp(shared_buffer_contents + start, xbox_data + start,
                      size) != 0 &&
          IsRangeValid(start, size)) {
        stale_valid.fetch_add(1, std::memory_order_relaxed);
      }
      if ((n & (n - 1)) == 0 || n % 100000 == 0) {
        REXLOG_INFO(
            "[shmem-valid-first] {}: direct-write runs {}, stale-valid {}",
            ShmemValidFirstEnabled() ? "on" : "off", n,
            stale_valid.load(std::memory_order_relaxed));
      }
    }
    command_processor_.RecordSharedMemoryUploadRoute(
        MetalCommandProcessor::SharedMemoryUploadRoute::kDirectWrite, size);
  };

  auto stage_upload_run = [&](uint32_t start, uint32_t end) -> bool {
    if (end <= start) {
      return true;
    }
    uint32_t offset = start;
    uint32_t remaining = end - start;
    while (remaining) {
      if (!blit_encoder) {
        command_processor_.NoteSharedMemoryUploadBreakCause(
            staged_has_current      ? PageAccess::kCurrent
            : staged_has_standalone ? PageAccess::kStandalone
            : staged_has_inflight   ? PageAccess::kInflight
                                    : PageAccess::kCompleted,
            !precommit_allowed || precommit_fell_back);
      }
      MTL::BlitCommandEncoder* encoder = get_blit_encoder();
      if (!encoder) {
        REXLOG_ERROR("MetalSharedMemory::UploadRanges: failed to get blit encoder");
        return false;
      }

      MTL::Buffer* upload_buffer = nullptr;
      size_t upload_offset = 0;
      uint64_t upload_gpu_address = 0;
      size_t upload_size = 0;
      uint8_t* upload_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), remaining, page_size,
          &upload_buffer, upload_offset, upload_gpu_address, upload_size);
      if (!upload_mapping || !upload_buffer || !upload_size) {
        REXLOG_ERROR(
            "MetalSharedMemory::UploadRanges: failed to allocate upload "
            "staging buffer");
        return false;
      }
      // RequestPartial rounds the size up to the page alignment. Runs are
      // page-aligned unless FABLE2_MEMEXPORT_PRESERVE carved them, so this
      // clamp is a no-op on the default path.
      upload_size = std::min<size_t>(upload_size, remaining);

      MakeRangeValid(offset, static_cast<uint32_t>(upload_size), false);
      copy_guest_bytes(upload_mapping, offset, upload_size);
      encoder->copyFromBuffer(upload_buffer,
                              static_cast<NS::UInteger>(upload_offset), buffer_,
                              static_cast<NS::UInteger>(offset),
                              static_cast<NS::UInteger>(upload_size));
      // The current command buffer writes these pages: a later pre-commit
      // copy or pre-commit texture load of them in this command buffer
      // would run before this copy, so classify them as current from now on.
      MarkGpuAccess(offset, static_cast<uint32_t>(upload_size),
                    command_processor_.GetCurrentSubmission());
      MarkGpuWrite(offset, static_cast<uint32_t>(upload_size),
                   command_processor_.GetCurrentSubmission(), true);
      command_processor_.RecordSharedMemoryUploadEncoderCopy();
      if (UploadVerifyEnabled()) {
        NoteUploadVerify(offset, static_cast<uint32_t>(upload_size), false);
      }
      command_processor_.RecordSharedMemoryUploadRoute(
          MetalCommandProcessor::SharedMemoryUploadRoute::kStagedBlit,
          upload_size);

      offset += static_cast<uint32_t>(upload_size);
      remaining -= static_cast<uint32_t>(upload_size);
    }
    return true;
  };

  auto precommit_upload_run = [&](const UploadRun& run) -> bool {
    // Only while the render pass that the in-command-buffer blit would end is
    // still open (an earlier in-pass run in this call may have ended it).
    MTL::BlitCommandEncoder* encoder =
        command_processor_.SharedMemoryPrecommitAllowed()
            ? command_processor_.BeginSharedMemoryPrecommit(run.wait)
            : nullptr;
    if (!encoder) {
      staged_has_inflight = true;
      precommit_fell_back = true;
      return stage_upload_run(run.start, run.end);
    }
    uint32_t offset = run.start;
    uint32_t remaining = run.end - run.start;
    while (remaining) {
      MTL::Buffer* upload_buffer = nullptr;
      size_t upload_offset = 0;
      uint64_t upload_gpu_address = 0;
      size_t upload_size = 0;
      // Tagged with the current submission: it completes only after this
      // pre-commit command buffer (it waits on its event before committing).
      uint8_t* upload_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), remaining, page_size,
          &upload_buffer, upload_offset, upload_gpu_address, upload_size);
      if (!upload_mapping || !upload_buffer || !upload_size) {
        REXLOG_ERROR(
            "MetalSharedMemory::UploadRanges: failed to allocate pre-commit "
            "staging buffer");
        return false;
      }
      upload_size = std::min<size_t>(upload_size, remaining);
      // Validate before copying (as the staged path and CR-02).
      MakeRangeValid(offset, static_cast<uint32_t>(upload_size), false);
      copy_guest_bytes(upload_mapping, offset, upload_size);
      encoder->copyFromBuffer(upload_buffer,
                              static_cast<NS::UInteger>(upload_offset), buffer_,
                              static_cast<NS::UInteger>(offset),
                              static_cast<NS::UInteger>(upload_size));
      command_processor_.RecordSharedMemoryUploadRoute(
          MetalCommandProcessor::SharedMemoryUploadRoute::kStagedBlit,
          upload_size);
      if (UploadVerifyEnabled()) {
        NoteUploadVerify(offset, static_cast<uint32_t>(upload_size), true);
      }
      MarkPrecommitCopy(offset, static_cast<uint32_t>(upload_size),
                        command_processor_.GetCurrentSubmission());
      offset += static_cast<uint32_t>(upload_size);
      remaining -= static_cast<uint32_t>(upload_size);
    }
    precommit_pages += ((run.end - 1) >> page_size_log2()) -
                       (run.start >> page_size_log2()) + 1;
    return true;
  };

  auto flush_merged_range_impl = [&](uint32_t start, uint32_t end) -> bool {
    if (end <= start) {
      return true;
    }
    std::vector<UploadRun> upload_runs;
    build_upload_runs(start, end, upload_runs);
    for (const UploadRun& run : upload_runs) {
      if (run.route == kRoutePrecommit) {
        if (!precommit_upload_run(run)) {
          return false;
        }
      } else if (REXCVAR_GET(metal_shared_memory_direct_write) &&
                 run.direct_safe) {
        direct_write_run(run.start, run.end);
      } else if (!stage_upload_run(run.start, run.end)) {
        return false;
      }
    }
    return true;
  };

  // FABLE2_MEMEXPORT_PRESERVE: carve still-valid memexport output out of the
  // upload. Unset, this is a direct call of the unchanged upload path.
  const bool preserve = MemexportPreserveEnabled() && !preserved_exports_.empty();
  uint64_t preserved_bytes = 0;
  if (preserve) {
    ExpirePreservedExports(PreserveNowUs());
  }
  auto flush_merged_range = [&](uint32_t start, uint32_t end) -> bool {
    const bool use_preserve = preserve && !preserved_exports_.empty();
    if (!use_preserve && pending_readbacks_.empty()) {
      return flush_merged_range_impl(start, end);
    }
    // Collect overlapping exports whose guest bytes are unchanged.
    std::vector<std::pair<uint32_t, uint32_t>> keep;
    for (size_t k = 0; use_preserve && k < preserved_exports_.size();) {
      const PreservedExport& e = preserved_exports_[k];
      if (e.end <= start || e.start >= end) {
        ++k;
        continue;
      }
      if (XXH3_64bits(xbox_data + e.start, e.end - e.start) != e.guest_hash) {
        // The CPU wrote into the export range: its data wins, stop preserving.
        ++preserve_dropped_;
        bool pending = false;
        for (const PendingReadback& r : pending_readbacks_) {
          if (r.end <= e.start || r.start >= e.end) continue;
          pending = true;
        }
        if (pending) ++preserve_dropped_pending_;
        if (preserve_dropped_logged_ < 24) {
          ++preserve_dropped_logged_;
          REXLOG_WARN(
              "[memexport-merge] re-upload #{}: pages {:08X}-{:08X} overlap "
              "export range {:08X}+{} whose guest bytes changed; uploading "
              "guest memory over it (readback pending: {})",
              preserve_dropped_logged_, start, end, e.start, e.end - e.start,
              pending ? "yes, export data is lost" : "no");
        }
        preserved_exports_.erase(preserved_exports_.begin() + k);
        continue;
      }
      keep.emplace_back(std::max(e.start, start), std::min(e.end, end));
      ++k;
    }
    // [memexport-hole] an export whose readback has not landed is only in the
    // buffer; guest RAM still holds the bytes from before the export. If no
    // preserve entry covers it (expired, displaced by an overlapping export,
    // list full, range too large), this upload puts the old bytes back.
    if (!pending_readbacks_.empty()) {
      const bool pending_keep = MemexportPendingKeepEnabled();
      const uint64_t now_us = PreserveNowUs();
      const size_t keep_preserved = keep.size();
      for (PendingReadback& r : pending_readbacks_) {
        if (r.end <= start || r.start >= end) continue;
        const uint32_t a = std::max(r.start, start);
        const uint32_t b = std::min(r.end, end);
        uint32_t uncovered = b - a;
        for (size_t k = 0; k < keep_preserved; ++k) {
          const uint32_t ka = std::max(keep[k].first, a);
          const uint32_t kb = std::min(keep[k].second, b);
          if (kb > ka) uncovered -= std::min(uncovered, kb - ka);
        }
        if (!uncovered) continue;
        const bool guest_same =
            XXH3_64bits(xbox_data + r.start, r.end - r.start) == r.guest_hash;
        const uint64_t age_us = now_us - r.export_us;
        const bool expired = age_us > kPreservedExportLifetimeUs;
        ++hole_events_;
        hole_bytes_ += uncovered;
        hole_events_guest_same_ += guest_same;
        hole_events_expired_ += expired;
        r.hole = true;
        const bool kept = pending_keep && guest_same;
        if (kept) {
          keep.emplace_back(a, b);
          hole_kept_bytes_ += uncovered;
        }
        if (hole_logged_ < 64) {
          ++hole_logged_;
          REXLOG_WARN(
              "[memexport-hole] #{}: upload {:08X}-{:08X} covers pending "
              "export {:08X}+{} (submission {}, {} ms old, {} bytes "
              "unprotected, guest bytes {} since the export): {}",
              hole_logged_, start, end, r.start, r.end - r.start, r.submission,
              age_us / 1000, uncovered, guest_same ? "unchanged" : "changed",
              kept ? "kept out of the upload"
                   : "guest memory uploaded over it");
        }
      }
    }
    if (keep.empty()) {
      return flush_merged_range_impl(start, end);
    }
    std::sort(keep.begin(), keep.end());
    uint32_t cursor = start;
    for (const auto& kr : keep) {
      if (kr.first > cursor && !flush_merged_range_impl(cursor, kr.first)) {
        return false;
      }
      uint32_t skip_start = std::max(cursor, kr.first);
      if (kr.second > skip_start) {
        // GPU data in the shared buffer is newer than guest RAM here.
        MakeRangeValid(skip_start, kr.second - skip_start, false);
        preserved_bytes += kr.second - skip_start;
      }
      cursor = std::max(cursor, kr.second);
    }
    if (cursor < end) {
      return flush_merged_range_impl(cursor, end);
    }
    return true;
  };

  for (uint32_t i = 0; i < num_upload_ranges; ++i) {
    const auto& range = upload_page_ranges[i];
    uint32_t start = range.first * page_size;
    uint32_t end = start + range.second * page_size;
    if (start >= kBufferSize) {
      continue;
    }
    if (end > kBufferSize) {
      end = kBufferSize;
    }

    if (!have_merged) {
      merged_start = start;
      merged_end = end;
      have_merged = true;
      continue;
    }

    // Merge overlapping/adjacent ranges.
    if (start <= merged_end) {
      if (end > merged_end) {
        merged_end = end;
      }
    } else {
      if (!flush_merged_range(merged_start, merged_end)) {
        command_processor_.EndSharedMemoryPrecommit(precommit_pages);
        command_processor_.EndSharedMemoryUploadBlitEncoder(
            MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
                kUploadFailure);
        return false;
      }
      merged_start = start;
      merged_end = end;
    }
  }

  if (have_merged) {
    if (!flush_merged_range(merged_start, merged_end)) {
      command_processor_.EndSharedMemoryPrecommit(precommit_pages);
      command_processor_.EndSharedMemoryUploadBlitEncoder(
          MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
              kUploadFailure);
      return false;
    }
  }

  // Commit the pre-commit copies now, ahead of the (still open) current
  // command buffer, and make the active render pass wait for them.
  command_processor_.EndSharedMemoryPrecommit(precommit_pages);

  if (preserved_bytes && PreserveTraceEnabled()) {
    frame_log::Record("tr_memexport_preserved_bytes", frame_log::CurrentFrame(),
                      preserved_bytes);
  }
  REXLOG_DEBUG("MetalSharedMemory::UploadRanges: Staged {} ranges to Metal buffer",
         num_upload_ranges);
  command_processor_.RecordSharedMemoryDirectWriteEligibility(
      direct_eligible_bytes, staged_required_bytes);

  return true;
}

void MetalSharedMemory::Shutdown() {
  pending_readbacks_.clear();
  upload_buffer_pool_.reset();
  if (buffer_) {
    buffer_->release();
    buffer_ = nullptr;
  }

  ShutdownCommon();  // Base class cleanup
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
