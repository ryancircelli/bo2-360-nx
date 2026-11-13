// Write stamps: when each 4 KB page of the guest's physical memory was last written, as far as we can observe it
// (milestone M1 of docs/research/07-d3d-renderer-plan.md).
//
// Why: the renderer copied ~6 MB of vertex data per frame from guest memory into GPU buffers, and a write watch on the
// PC showed that 98 % of those bytes came from pages nothing wrote during 45 s of Town (the static model and world
// geometry). A persistent GPU copy of a range stays correct as long as none of its pages was written since the copy
// was made, and the stamps answer that without reading the bytes (hashing them cost as much as copying them).
//
// Who stamps: the game's copy and fill routines (all native, bo2zm_crt.cpp: the asset loader's copies, the UP draws'
// vertices), the CPU skinning hooks (their output), the renderer's memexport (streamed texture data). Writers with
// plain stores we do not hook are caught by the renderer's sampled check, which then marks the range's pages as
// always changing (MarkVolatile).
//
// Who reads them: the vertex cache (M1) and the texture rechecks (M3, bo2zm_native_texture_by_writes).
//
// The stamp is an epoch the renderer advances once per frame (NextEpoch). A copy made in epoch E stays valid while
// every page in its range has a stamp below E: a write in the same epoch, before or after the copy, makes it stale
// (a copy too many, never a stale one).

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace bo2zm::page_writes {

// On with bo2zm_native_vertex_cache_by_writes or bo2zm_native_texture_by_writes: stamps are only recorded while
// someone uses them.
extern std::atomic<bool> g_enabled;
extern std::atomic<uint32_t> g_epoch;
extern std::atomic<uint32_t> g_stamps[1u << 17];  // 512 MB of physical memory in 4 KB pages
// The newest stamp of each 64 KB region (16 pages), never below any of its pages' stamps. Unchanged looks
// here first: 32 KB that stay in the cache, where the page stamps (512 KB, read at scattered places) missed it on
// most lookups. A region with nothing stamped since answers for all its pages. g_use_regions: the A/B switch.
extern std::atomic<uint32_t> g_regions[1u << 13];
extern std::atomic<bool> g_use_regions;

// Out of line on purpose: MarkGuest is inlined into the game's copy and fill hooks, which LTO inlines into thousands
// of recompiled callers; with the stamping loops inline the image grew by 1.75 MB.
[[gnu::noinline]] void MarkPhysical(uint32_t physical, uint32_t bytes);

// A guest address: only the views of physical memory (0xA0000000 and up) are stamped. The 0xE0000000 view starts at
// physical 0x1000 (PhysicalHeap::GetPhysicalAddress); without that offset its writes were stamped one page low.
inline void MarkGuest(uint32_t guest, uint32_t bytes) {
  if (guest >= 0xA0000000u && g_enabled.load(std::memory_order_relaxed)) [[unlikely]] {
    MarkPhysical((guest & 0x1FFFFFFFu) + (guest >= 0xE0000000u ? 0x1000u : 0u), bytes);
  }
}

// The range's pages count as written in every epoch from now on.
void MarkVolatile(uint32_t physical, uint32_t bytes);

// True when no page of [physical, physical + bytes) was stamped in epoch `since` or later.
inline bool Unchanged(uint32_t physical, uint32_t bytes, uint32_t since) {
  if (bytes == 0) {
    return true;
  }
  const uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
  const uint32_t last = std::min<uint32_t>(((physical & 0x1FFFFFFFu) + bytes - 1) >> 12, (1u << 17) - 1);
  if (g_use_regions.load(std::memory_order_relaxed)) {
    for (uint32_t r = first >> 4; r <= last >> 4; ++r) {
      if (g_regions[r].load(std::memory_order_acquire) < since) {
        continue;  // nothing stamped in this region since
      }
      const uint32_t hi = std::min(last, (r << 4) + 15);
      for (uint32_t p = std::max(first, r << 4); p <= hi; ++p) {
        if (g_stamps[p].load(std::memory_order_acquire) >= since) {
          return false;
        }
      }
    }
    return true;
  }
  for (uint32_t p = first; p <= last; ++p) {
    if (g_stamps[p].load(std::memory_order_acquire) >= since) {
      return false;
    }
  }
  return true;
}

inline uint32_t Epoch() { return g_epoch.load(std::memory_order_relaxed); }
void NextEpoch();
void Enable(bool on);

}  // namespace bo2zm::page_writes
