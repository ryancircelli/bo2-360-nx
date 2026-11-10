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
//
// REX_METAL_TEX_CONTENT_REPAIR=<N> (0 = off) runs the same check every N
// submissions (when REX_METAL_TEX_CONTENT_CHECK is not set) and, on a
// mismatch the buffer does not explain, invalidates the range as a CPU write
// would have: the texture watches fire and the next draw reloads the texture
// from guest memory. A safety net for writes the page watches miss.

#include "rex/graphics/metal/metal_texture_cache.h"

#include <cstdlib>

#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/logging.h"
#include "xxhash.h"

namespace rex {
namespace graphics {
namespace metal {

uint32_t MetalTextureCache::TexContentRepairInterval() {
  static const uint32_t n = [] {
    const char* v = MetalDebugEnv("TEX_CONTENT_REPAIR");
    return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
  }();
  return n;
}

uint32_t MetalTextureCache::TexContentCheckInterval() {
  static const uint32_t n = [] {
    const char* v = MetalDebugEnv("TEX_CONTENT_CHECK");
    uint32_t r = v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
    if (!r) r = TexContentRepairInterval();
    if (r) {
      REXLOG_INFO("[tex-content-check] on, every {} submissions "
                  "(REX_METAL_TEX_CONTENT_CHECK), repair {} "
                  "(REX_METAL_TEX_CONTENT_REPAIR)",
                  r, TexContentRepairInterval() ? "on" : "off");
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
        if (g == t->cc_hash_[i]) continue;
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
        // [tex-content-repair] A range the buffer does not hold as GPU data
        // (category 1 may be a resolve or memexport result that guest memory
        // never receives) is invalidated as a CPU write would have done: the
        // watches fire, the texture becomes outdated and the next draw that
        // uses it loads guest memory again.
        bool repaired = false;
        if (cat != 1 && TexContentRepairInterval()) {
          shared_memory().MemoryInvalidationCallback(r.start, r.length, true);
          ++cc_repaired_;
          repaired = true;
        }
        const uint64_t total = cc_mismatch_[0] + cc_mismatch_[1] +
                               cc_mismatch_[2];
        if (total <= 64 || (total & (total - 1)) == 0) {
          const auto& key = t->key();
          REXLOG_WARN(
              "[tex-content-check] mismatch #{} ({} #{}): {} {:08X}+{} of "
              "base {:08X} mips {:08X} fmt {} {}x{}x{} dim {} pitch {} "
              "tiled {} packed {} levels {} route {} loaded {} submissions "
              "ago, shmem valid {}, guest==buffer {}, rtt {} nx {}{}",
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
              t->rtt_tracked_, t->nx_tracked_,
              repaired ? ", invalidated for a reload" : "");
        }
      }
    }
  }
  if (submission >= cc_last_summary_ + 600) {
    cc_last_summary_ = submission;
    REXLOG_INFO(
        "[tex-content-check] summary at submission {}: checked {}, mismatches "
        "neither {} buffer-only {} stale-load {}, loads that read stale data "
        "{}, invalidated for a reload {}",
        submission, cc_checked_, cc_mismatch_[0], cc_mismatch_[1],
        cc_mismatch_[2], cc_stale_loads_, cc_repaired_);
  }
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
