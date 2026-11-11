// [tex-content-check] Debug verifier for texture contents.
//
// REX_METAL_TEX_CONTENT_CHECK=<N> (0 = off, the default). After every texture
// load, the guest bytes the load read are hashed per range (base, mips); for
// resident loads these are the bytes the shared memory buffer was uploaded
// from, as long as the range is still valid. Every
// N submissions, each texture bound by a draw is checked once: for a range the
// cache considers up to date, guest memory is hashed again and compared with
// the load hash. A mismatch means the GPU copy differs from what the guest has
// now while the cache believes it is current: a CPU write the watches missed,
// or a load that read stale data. Each mismatch logs the address, format,
// size and mips; a summary line with the counters is logged every 600
// submissions.

#include "rex/graphics/metal/metal_texture_cache.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/logging.h"
#include "rex/system/xmemory.h"
#include "xxhash.h"

namespace rex {
namespace graphics {
namespace metal {

uint32_t MetalTextureCache::TexContentCheckInterval() {
  static const uint32_t n = [] {
    const char* v = MetalDebugEnv("TEX_CONTENT_CHECK");
    uint32_t r = v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
    if (r) {
      REXLOG_INFO("[tex-content-check] on, every {} submissions "
                  "(REX_METAL_TEX_CONTENT_CHECK)", r);
    }
    return r;
  }();
  return n;
}

namespace {
struct CcRange {
  uint32_t start;
  uint32_t length;
};
}  // namespace

template <typename T>
static CcRange CcGetRange(const T& texture, uint32_t i) {
  const auto& key = texture.key();
  if (i == 0) {
    return {uint32_t(key.base_page) << 12, texture.GetGuestBaseSize()};
  }
  return {uint32_t(key.mip_page) << 12, texture.GetGuestMipsSize()};
}

void MetalTextureCache::TexContentRecord(MetalTexture& texture, bool base,
                                         bool mips, uint8_t route) {
  if (texture.key().scaled_resolve) return;
  auto& shm = static_cast<MetalSharedMemory&>(shared_memory());
  const uint8_t* ram = shm.GetXboxRamBase();
  if (!ram) return;
  const uint64_t submission =
      command_processor_ ? command_processor_->GetCurrentSubmission() : 0;
  auto global_lock = AcquireGlobalLock();
  for (uint32_t i = 0; i < 2; ++i) {
    if (!(i ? mips : base)) continue;
    const CcRange r = CcGetRange(texture, i);
    if (!r.length) {
      texture.cc_have_[i] = false;
      continue;
    }
    // Shared memory uploads are staged blits that may not have run yet, so
    // the buffer's CPU view is not what a resident load reads. A range that
    // is still valid holds the guest bytes of its last upload and has not
    // been written since (a write invalidates it), so guest memory is the
    // load's source for both routes. A resident range that is no longer
    // valid was written after its upload: the load may have read old data.
    const uint64_t g = XXH3_64bits(ram + r.start, r.length);
    const bool stale =
        route == 1 && !shared_memory().IsRangeValid(r.start, r.length);
    texture.cc_hash_[i] = g;
    texture.cc_have_[i] = true;
    texture.cc_route_[i] = route;
    texture.cc_load_submission_[i] = submission;
    texture.cc_stale_at_load_[i] = stale;
    if (stale) ++cc_stale_loads_;
  }
}

void MetalTextureCache::TexContentMarkCurrent(MetalTexture& texture,
                                              uint32_t i) {
  // Called under the global lock from LoadedRangeStillCurrent, the one point
  // every load route (single, batched, CPU-source, resolve) passes when a
  // range is marked up to date. Watched pages cannot change while the lock is
  // held, so guest memory now is the data the texture is meant to hold.
  if (texture.key().scaled_resolve) {
    texture.cc_have_[i] = false;
    return;
  }
  const uint8_t* ram =
      static_cast<MetalSharedMemory&>(shared_memory()).GetXboxRamBase();
  const CcRange r = CcGetRange(texture, i);
  if (!ram || !r.length) {
    texture.cc_have_[i] = false;
    return;
  }
  const uint64_t g = XXH3_64bits(ram + r.start, r.length);
  if (texture.cc_have_[i] && texture.cc_hash_[i] != g &&
      !texture.cc_stale_at_load_[i]) {
    // The load recorded different bytes than the ones now being marked
    // current: the data changed between the load and this point.
    texture.cc_stale_at_load_[i] = true;
    ++cc_stale_loads_;
  }
  texture.cc_hash_[i] = g;
  texture.cc_have_[i] = true;
  texture.cc_load_submission_[i] =
      command_processor_ ? command_processor_->GetCurrentSubmission() : 0;
}

void MetalTextureCache::TexContentCheck(uint32_t used_texture_mask) {
  const uint32_t interval = TexContentCheckInterval();
  if (!interval || !command_processor_) return;
  const uint64_t submission = command_processor_->GetCurrentSubmission();
  const uint64_t window = submission / interval;
  auto& shm = static_cast<MetalSharedMemory&>(shared_memory());
  const uint8_t* ram = shm.GetXboxRamBase();
  const uint8_t* buf =
      shm.GetBuffer() ? static_cast<const uint8_t*>(shm.GetBuffer()->contents())
                      : nullptr;
  if (!ram) return;
  uint32_t remaining = used_texture_mask;
  uint32_t index = 0;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining = rex::clear_lowest_bit(remaining);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) continue;
    for (Texture* tex : {binding->texture, binding->texture_signed}) {
      if (!tex) continue;
      auto* t = static_cast<MetalTexture*>(tex);
      if (t->cc_window_ == window) continue;
      t->cc_window_ = window;
      // Hold the global lock: a CPU write to a watched page blocks in the
      // access handler until the check is done, so only unwatched writes can
      // make guest memory differ from an up-to-date texture.
      auto global_lock = AcquireGlobalLock();
      const bool outdated[2] = {t->base_outdated(global_lock),
                                t->mips_outdated(global_lock)};
      for (uint32_t i = 0; i < 2; ++i) {
        if (!t->cc_have_[i] || outdated[i]) continue;
        const CcRange r = CcGetRange(*t, i);
        if (!r.length) continue;
        ++cc_checked_;
        const uint64_t g = XXH3_64bits(ram + r.start, r.length);
        if (g == t->cc_hash_[i]) {
          // Guest memory is unchanged. A resident load copied from the
          // shared buffer, so the buffer must hold the same bytes once the
          // load's command buffer has completed. If it does not, the texture
          // was built from bytes that are not the guest's: the buffer was
          // stale while marked valid, or a memory export wrote over it.
          if (buf && t->cc_route_[i] == 1 &&
              t->cc_load_submission_[i] + 2 <=
                  command_processor_->GetCompletedSubmission() &&
              shared_memory().IsRangeValid(r.start, r.length) &&
              XXH3_64bits(buf + r.start, r.length) != g) {
            static std::unordered_set<uint64_t> seen;
            ++cc_gpu_differs_;
            if (seen.insert((uint64_t(r.start) << 32) | r.length).second) {
              ++cc_gpu_differs_unique_;
              if (cc_gpu_differs_unique_ <= 32) {
                uint32_t pages = 0, bad = 0;
                for (uint32_t o = 0; o < r.length; o += 4096, ++pages) {
                  const uint32_t n = std::min<uint32_t>(4096, r.length - o);
                  if (std::memcmp(buf + r.start + o, ram + r.start + o, n))
                    ++bad;
                }
                const auto& key = t->key();
                REXLOG_WARN(
                    "[tex-content-check] gpu-differs #{}: {} {:08X}+{} fmt {} "
                    "{}x{} levels {}: {} of {} pages differ from guest "
                    "memory, loaded {} submissions ago; watch state {}",
                    cc_gpu_differs_unique_, i ? "mips" : "base", r.start,
                    r.length, uint32_t(key.format), key.GetWidth(),
                    key.GetHeight(), uint32_t(key.mip_max_level) + 1, bad,
                    pages, submission - t->cc_load_submission_[i],
                    shm.DescribeWatchState(r.start, r.length));
              }
            }
          }
          continue;
        }
        // 0: texture matches neither guest memory nor the buffer.
        // 1: texture matches the shared buffer, guest memory differs (a
        //    GPU-written range, or a CPU write the buffer missed too).
        // 2: the load already read data that differed from guest memory.
        const uint64_t b = buf ? XXH3_64bits(buf + r.start, r.length) : 0;
        const uint32_t cat = t->cc_stale_at_load_[i]   ? 2u
                             : (b == t->cc_hash_[i]) ? 1u
                                                     : 0u;
        const uint64_t n = ++cc_mismatch_[cat];
        // Log each texture range once until it is reloaded.
        t->cc_have_[i] = false;
        const uint64_t total = cc_mismatch_[0] + cc_mismatch_[1] +
                               cc_mismatch_[2];
        if (total <= 64 || (total & (total - 1)) == 0) {
          const auto& key = t->key();
          REXLOG_WARN(
              "[tex-content-check] mismatch #{} ({} #{}): {} {:08X}+{} of "
              "base {:08X} mips {:08X} fmt {} {}x{}x{} dim {} pitch {} "
              "tiled {} packed {} levels {} route {} loaded {} submissions "
              "ago, shmem valid {}, guest==buffer {}, rtt {} nx {}",
              total, cat == 0 ? "neither" : cat == 1 ? "buffer-only" : "stale-load",
              n, i ? "mips" : "base", r.start, r.length,
              uint32_t(key.base_page) << 12, uint32_t(key.mip_page) << 12,
              uint32_t(key.format), key.GetWidth(), key.GetHeight(),
              key.GetDepthOrArraySize(), uint32_t(key.dimension),
              uint32_t(key.pitch), uint32_t(key.tiled),
              uint32_t(key.packed_mips), uint32_t(key.mip_max_level) + 1,
              t->cc_route_[i] == 1 ? "resident" : "cpu",
              submission - t->cc_load_submission_[i],
              shared_memory().IsRangeValid(r.start, r.length), g == b,
              t->rtt_tracked_, t->nx_tracked_);
          // Which view still has the range watched, and whether the host
          // page is really read only there: w/- watch flag, W/R/N host
          // protection, then the guest pages of that host page.
          REXLOG_WARN("[tex-content-check] watch state of mismatch #{}: {}",
                      total,
                      shm.DescribeWatchState(r.start, r.length));
        }
      }
    }
  }
  if (submission >= cc_last_summary_ + 600) {
    cc_last_summary_ = submission;
    REXLOG_INFO(
        "[tex-content-check] summary at submission {}: checked {}, mismatches "
        "neither {} buffer-only {} stale-load {}, loads that read stale data "
        "{}; gpu-differs {} ({} ranges); watch faults {}",
        submission, cc_checked_, cc_mismatch_[0], cc_mismatch_[1],
        cc_mismatch_[2], cc_stale_loads_, cc_gpu_differs_,
        cc_gpu_differs_unique_,
        rex::memory::Memory::DescribeWatchFaults());
  }
}


// [tex-garbage] Picture-independent corruption metric.
//
// REX_METAL_TEX_GARBAGE_SCAN=<N> (0 = off, the default). Every N submissions
// each bound DXT1 / DXT2_3 / DXT4_5 texture is scanned once: its base range
// in guest memory is cut into 128-byte chunks (the granularity of the game's
// paired block moves), and a chunk is "bad" when at least half of its colour
// blocks have endpoints far apart, which is what data of another format or
// type decodes to (real colour blocks have endpoints close together). A
// texture with enough bad chunks is logged with its address, format and size,
// the same count for the shared buffer's copy, and the even / odd chunk split.
// No lock is taken and nothing is changed, so unlike the content check this
// does not hold up CPU writers.
uint32_t MetalTextureCache::TexGarbageScanInterval() {
  static const uint32_t n = [] {
    const char* v = MetalDebugEnv("TEX_GARBAGE_SCAN");
    uint32_t r = v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
    if (r) {
      REXLOG_INFO("[tex-garbage] on, every {} submissions "
                  "(REX_METAL_TEX_GARBAGE_SCAN)", r);
    }
    return r;
  }();
  return n;
}

namespace {
struct GsCount {
  uint32_t chunks = 0;
  uint32_t bad = 0;
  uint32_t bad_even = 0;
  uint32_t bad_odd = 0;
};

// Colour endpoints of one block, undoing the texture's byte order.
inline void GsEndpoints(const uint8_t* p, xenos::Endian endian, uint32_t& c0,
                        uint32_t& c1) {
  uint8_t b[4] = {p[0], p[1], p[2], p[3]};
  switch (endian) {
    case xenos::Endian::k8in16:
      std::swap(b[0], b[1]);
      std::swap(b[2], b[3]);
      break;
    case xenos::Endian::k8in32:
      std::swap(b[0], b[3]);
      std::swap(b[1], b[2]);
      break;
    case xenos::Endian::k16in32:
      std::swap(b[0], b[2]);
      std::swap(b[1], b[3]);
      break;
    default:
      break;
  }
  c0 = uint32_t(b[0]) | (uint32_t(b[1]) << 8);
  c1 = uint32_t(b[2]) | (uint32_t(b[3]) << 8);
}

inline bool GsWild(uint32_t c0, uint32_t c1) {
  const int r = int((c0 >> 11) & 31) - int((c1 >> 11) & 31);
  const int g = int((c0 >> 5) & 63) - int((c1 >> 5) & 63);
  const int b = int(c0 & 31) - int(c1 & 31);
  return std::abs(r) * 8 + std::abs(g) * 4 + std::abs(b) * 8 >= 256;
}

GsCount GsScan(const uint8_t* data, uint32_t length, uint32_t block_bytes,
               uint32_t colour_offset, xenos::Endian endian) {
  GsCount out;
  constexpr uint32_t kChunk = 128;
  const uint32_t chunks = length / kChunk;
  // At most about 512 chunks per texture, spread over the whole range; an odd
  // step so both chunk parities are sampled.
  uint32_t step = std::max<uint32_t>(1, chunks / 512);
  step |= 1;
  const uint32_t blocks = kChunk / block_bytes;
  for (uint32_t c = 0; c < chunks; c += step) {
    const uint8_t* p = data + size_t(c) * kChunk;
    uint32_t wild = 0;
    for (uint32_t k = 0; k < blocks; ++k) {
      uint32_t c0, c1;
      GsEndpoints(p + k * block_bytes + colour_offset, endian, c0, c1);
      wild += GsWild(c0, c1);
    }
    ++out.chunks;
    if (wild * 2 >= blocks) {
      ++out.bad;
      ++((c & 1) ? out.bad_odd : out.bad_even);
    }
  }
  return out;
}
}  // namespace

void MetalTextureCache::TexGarbageScan(uint32_t used_texture_mask) {
  const uint32_t interval = TexGarbageScanInterval();
  if (!interval || !command_processor_) return;
  const uint64_t submission = command_processor_->GetCurrentSubmission();
  const uint64_t window = submission / interval;
  auto& shm = static_cast<MetalSharedMemory&>(shared_memory());
  const uint8_t* ram = shm.GetXboxRamBase();
  const uint8_t* buf =
      shm.GetBuffer() ? static_cast<const uint8_t*>(shm.GetBuffer()->contents())
                      : nullptr;
  if (!ram) return;
  if (gs_window_current_ != window) {
    if (gs_window_current_ != UINT64_MAX) {
      gs_window_flagged_max_ =
          std::max(gs_window_flagged_max_, gs_window_flagged_);
      gs_window_flagged_sum_ += gs_window_flagged_;
    }
    gs_window_current_ = window;
    gs_window_flagged_ = 0;
  }
  uint32_t remaining = used_texture_mask;
  uint32_t index = 0;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining = rex::clear_lowest_bit(remaining);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding || !binding->texture) continue;
    auto* t = static_cast<MetalTexture*>(binding->texture);
    if (t->gs_window_ == window) continue;
    t->gs_window_ = window;
    const auto& key = t->key();
    if (key.scaled_resolve) continue;
    uint32_t block_bytes, colour_offset;
    switch (key.format) {
      case xenos::TextureFormat::k_DXT1:
        block_bytes = 8;
        colour_offset = 0;
        break;
      case xenos::TextureFormat::k_DXT2_3:
      case xenos::TextureFormat::k_DXT4_5:
        block_bytes = 16;
        colour_offset = 8;
        break;
      default:
        continue;
    }
    const CcRange r = CcGetRange(*t, 0);
    if (r.length < 2048) continue;
    ++gs_scanned_;
    const GsCount g =
        GsScan(ram + r.start, r.length, block_bytes, colour_offset,
               key.endianness);
    // Flag: a tenth of the sampled chunks, and at least four.
    if (g.bad < 4 || g.bad * 10 < g.chunks) {
      t->gs_logged_bad_ = 0;
      continue;
    }
    ++gs_window_flagged_;
    if (t->gs_logged_bad_) continue;  // already logged, still bad
    t->gs_logged_bad_ = g.bad;
    ++gs_flag_events_;
    static std::unordered_set<uint64_t> seen;
    const bool first = seen.insert((uint64_t(r.start) << 32) | r.length).second;
    if (first) ++gs_flag_unique_;
    if (gs_flag_events_ <= 400) {
      GsCount b;
      if (buf) {
        b = GsScan(buf + r.start, r.length, block_bytes, colour_offset,
                   key.endianness);
      }
      REXLOG_WARN(
          "[tex-garbage] #{} at submission {}: base {:08X}+{} mips {:08X} fmt "
          "{} {}x{} levels {} pitch {} tiled {} packed {}: bad chunks {} of "
          "{} (even {} odd {}), in the shared buffer {} of {} (even {} odd "
          "{}), guest==buffer {}{}",
          gs_flag_events_, submission, r.start, r.length,
          uint32_t(key.mip_page) << 12, uint32_t(key.format), key.GetWidth(),
          key.GetHeight(), uint32_t(key.mip_max_level) + 1,
          uint32_t(key.pitch), uint32_t(key.tiled), uint32_t(key.packed_mips),
          g.bad, g.chunks, g.bad_even, g.bad_odd, b.bad, b.chunks, b.bad_even,
          b.bad_odd,
          buf ? std::memcmp(buf + r.start, ram + r.start, r.length) == 0
              : false,
          first ? "" : " (range seen before)");
    }
  }
  if (submission >= gs_last_summary_ + 600) {
    gs_last_summary_ = submission;
    REXLOG_INFO(
        "[tex-garbage] summary at submission {}: scanned {}, flagged events "
        "{} (ranges {}), flagged textures bound per window: now {} max {} sum "
        "{}",
        submission, gs_scanned_, gs_flag_events_, gs_flag_unique_,
        gs_window_flagged_, gs_window_flagged_max_, gs_window_flagged_sum_);
  }
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
