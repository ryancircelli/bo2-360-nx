// See bo2zm_page_writes.h.

#include "bo2zm_page_writes.h"

#include <algorithm>

namespace bo2zm::page_writes {

std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_epoch{1};
std::atomic<uint32_t> g_stamps[1u << 17] = {};
std::atomic<uint32_t> g_regions[1u << 13] = {};
std::atomic<bool> g_use_regions{true};

void MarkPhysical(uint32_t physical, uint32_t bytes) {
  if (bytes == 0) {
    return;
  }
  const uint32_t epoch = g_epoch.load(std::memory_order_relaxed);
  const uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
  const uint32_t last = std::min<uint32_t>(((physical & 0x1FFFFFFFu) + bytes - 1) >> 12, (1u << 17) - 1);
  for (uint32_t p = first; p <= last; ++p) {
    // Never lower a stamp (a page marked volatile keeps UINT32_MAX).
    if (g_stamps[p].load(std::memory_order_relaxed) < epoch) {
      g_stamps[p].store(epoch, std::memory_order_release);
    }
  }
  for (uint32_t r = first >> 4; r <= last >> 4; ++r) {
    if (g_regions[r].load(std::memory_order_relaxed) < epoch) {
      g_regions[r].store(epoch, std::memory_order_release);
    }
  }
}

void MarkVolatile(uint32_t physical, uint32_t bytes) {
  if (bytes == 0) {
    return;
  }
  const uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
  const uint32_t last = std::min<uint32_t>(((physical & 0x1FFFFFFFu) + bytes - 1) >> 12, (1u << 17) - 1);
  for (uint32_t p = first; p <= last; ++p) {
    g_stamps[p].store(UINT32_MAX, std::memory_order_release);
  }
  for (uint32_t r = first >> 4; r <= last >> 4; ++r) {
    g_regions[r].store(UINT32_MAX, std::memory_order_release);
  }
}

void NextEpoch() {
  g_epoch.fetch_add(1, std::memory_order_relaxed);
}

void Enable(bool on) {
  g_enabled.store(on, std::memory_order_relaxed);
}

}  // namespace bo2zm::page_writes
