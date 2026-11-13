// Switch test build only: the C heap's live bytes by caller, to find what makes it grow.
//
// Why: in a long run at round 15, newlib's heap in use grew ~8.4 MB a minute for 25 minutes while the GPU memory
// stayed flat. Judging from the rates that is probably about 1.8 KB per texture created, but that is a correlation,
// not a known cause.
//
// What: the linker routes malloc, free, calloc, realloc, memalign, aligned_alloc and posix_memalign to the wrappers
// below (-Wl,--wrap=..., app/CMakeLists.txt, only with BO2ZM_HEAP_TRACK: BO2_HEAP_TRACK=1 bash app/build_switch.sh).
// One allocation in 16, chosen by a hash of its address (so free finds the same ones), is noted with its caller: the
// return address and three more from the frame records (this file is built with frame pointers; the walk stops at a
// record that does not look like one). Once a minute the frame hook logs the callers whose live bytes grew most, and
// the largest, times 16, with image offsets for addr2line on the ELF. Allocations made inside newlib through its
// reentrant names (_malloc_r: stdio, strdup) are not seen; frees of those are ignored.

#include "bo2zm_heap_track.h"

#if defined(BO2ZM_HEAP_TRACK)

#include <malloc.h>
#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/logging.h>

extern "C" {
void* __real_malloc(size_t size);
void __real_free(void* p);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* p, size_t size);
void* __real_memalign(size_t alignment, size_t size);
void* __real_aligned_alloc(size_t alignment, size_t size);
int __real_posix_memalign(void** out, size_t alignment, size_t size);
void _start(void);
}

namespace {

constexpr uint64_t kSampleMask = 15;     // one allocation in 16
constexpr uint32_t kSlots = 1u << 19;    // sampled live allocations (8 MB)
constexpr uint32_t kSites = 1u << 13;    // callers
constexpr int kDepth = 4;
constexpr auto kReportEvery = std::chrono::seconds(60);

struct Slot {
  uintptr_t ptr;  // 0 = empty
  uint32_t site;
  uint32_t size;
};
struct Site {
  uint64_t pcs[kDepth];  // pcs[0] == 0: empty
  int64_t bytes;
  int64_t count;
  uint32_t size;  // the last one noted
};

Slot g_slots[kSlots];
Site g_sites[kSites];
uint32_t g_live = 0;
uint64_t g_untracked = 0;  // sampled, but no room
Mutex g_mutex = 0;          // libnx's: no allocation, sleeps in the kernel

uint64_t Mix(uintptr_t p) {
  uint64_t x = p;
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  return x;
}
bool Sampled(const void* p) {
  return p && (Mix(uintptr_t(p)) & kSampleMask) == 0;
}
uint32_t Home(uintptr_t p) {
  return uint32_t(Mix(p) >> 20) & (kSlots - 1);
}

// The return addresses above the wrapper's own frame record (fp): its caller, then the frame records' chain.
void Walk(uintptr_t fp, uint64_t* pcs) {
  for (int i = 0; i < kDepth; ++i) {
    pcs[i] = 0;
  }
  for (int i = 0; i < kDepth; ++i) {
    const uintptr_t record = fp;
    pcs[i] = reinterpret_cast<const uint64_t*>(record)[1];
    fp = reinterpret_cast<const uintptr_t*>(record)[0];
    if ((fp & 15) != 0 || fp <= record || fp - record > 0x100000) {
      break;  // not a frame record above this one
    }
  }
}

uint32_t FindSite(const uint64_t* pcs) {
  uint64_t h = 0;
  for (int i = 0; i < kDepth; ++i) {
    h = Mix(h ^ pcs[i]);
  }
  uint32_t i = uint32_t(h) & (kSites - 1);
  for (uint32_t probe = 0; probe < kSites; ++probe, i = (i + 1) & (kSites - 1)) {
    Site& s = g_sites[i];
    if (s.pcs[0] == 0) {
      std::memcpy(s.pcs, pcs, sizeof(s.pcs));
      return i;
    }
    if (std::memcmp(s.pcs, pcs, sizeof(s.pcs)) == 0) {
      return i;
    }
  }
  return 0;  // full: everything else on slot 0's caller (does not happen with 8192)
}

void Note(void* p, size_t size, uintptr_t fp) {
  uint64_t pcs[kDepth];
  Walk(fp, pcs);
  mutexLock(&g_mutex);
  if (g_live >= kSlots / 4 * 3) {
    ++g_untracked;
    mutexUnlock(&g_mutex);
    return;
  }
  uint32_t i = Home(uintptr_t(p));
  while (g_slots[i].ptr != 0) {
    if (g_slots[i].ptr == uintptr_t(p)) {  // noted already (one wrapped call inside another)
      mutexUnlock(&g_mutex);
      return;
    }
    i = (i + 1) & (kSlots - 1);
  }
  const uint32_t site = FindSite(pcs);
  g_slots[i] = {uintptr_t(p), site, uint32_t(std::min<size_t>(size, UINT32_MAX))};
  ++g_live;
  g_sites[site].bytes += int64_t(size);
  ++g_sites[site].count;
  g_sites[site].size = uint32_t(std::min<size_t>(size, UINT32_MAX));
  mutexUnlock(&g_mutex);
}

void Forget(void* p) {
  mutexLock(&g_mutex);
  uint32_t i = Home(uintptr_t(p));
  while (g_slots[i].ptr != 0 && g_slots[i].ptr != uintptr_t(p)) {
    i = (i + 1) & (kSlots - 1);
  }
  if (g_slots[i].ptr == 0) {
    mutexUnlock(&g_mutex);
    return;  // not noted (allocated through newlib's own names)
  }
  Site& site = g_sites[g_slots[i].site];
  site.bytes -= int64_t(g_slots[i].size);
  --site.count;
  --g_live;
  // Linear probing: the slots after it that would no longer be found move back into the hole.
  uint32_t j = i;
  for (;;) {
    j = (j + 1) & (kSlots - 1);
    if (g_slots[j].ptr == 0) {
      break;
    }
    const uint32_t k = Home(g_slots[j].ptr);
    const bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
    if (stays) {
      continue;
    }
    g_slots[i] = g_slots[j];
    i = j;
  }
  g_slots[i] = {};
  mutexUnlock(&g_mutex);
}

}  // namespace

extern "C" {

__attribute__((used, noinline, optimize("no-omit-frame-pointer"))) void* __wrap_malloc(size_t size) {
  void* p = __real_malloc(size);
  if (Sampled(p)) {
    Note(p, size, uintptr_t(__builtin_frame_address(0)));
  }
  return p;
}

__attribute__((used, noinline, optimize("no-omit-frame-pointer"))) void __wrap_free(void* p) {
  if (Sampled(p)) {
    Forget(p);
  }
  __real_free(p);
}

__attribute__((used, noinline, optimize("no-omit-frame-pointer"))) void* __wrap_calloc(size_t count, size_t size) {
  void* p = __real_calloc(count, size);
  if (Sampled(p)) {
    Note(p, count * size, uintptr_t(__builtin_frame_address(0)));
  }
  return p;
}

__attribute__((used, noinline, optimize("no-omit-frame-pointer"))) void* __wrap_realloc(void* old, size_t size) {
  if (Sampled(old)) {
    Forget(old);
  }
  void* p = __real_realloc(old, size);
  if (Sampled(p)) {
    Note(p, size, uintptr_t(__builtin_frame_address(0)));
  }
  return p;
}

__attribute__((used, noinline, optimize("no-omit-frame-pointer"))) void* __wrap_memalign(size_t alignment, size_t size) {
  void* p = __real_memalign(alignment, size);
  if (Sampled(p)) {
    Note(p, size, uintptr_t(__builtin_frame_address(0)));
  }
  return p;
}

__attribute__((used, noinline, optimize("no-omit-frame-pointer"))) void* __wrap_aligned_alloc(size_t alignment, size_t size) {
  void* p = __real_aligned_alloc(alignment, size);
  if (Sampled(p)) {
    Note(p, size, uintptr_t(__builtin_frame_address(0)));
  }
  return p;
}

__attribute__((used, noinline, optimize("no-omit-frame-pointer"))) int __wrap_posix_memalign(void** out, size_t alignment, size_t size) {
  const int result = __real_posix_memalign(out, alignment, size);
  if (result == 0 && Sampled(*out)) {
    Note(*out, size, uintptr_t(__builtin_frame_address(0)));
  }
  return result;
}

}  // extern "C"

void bo2zm::heap_track::EveryFrame() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last = Clock::now();
  const auto now = Clock::now();
  if (now - last < kReportEvery) [[likely]] {
    return;
  }
  last = now;
  // Copied under the lock; logged (which allocates) after it.
  static Site sites[kSites];
  static int64_t before[kSites];
  mutexLock(&g_mutex);
  std::memcpy(sites, g_sites, sizeof(sites));
  const uint32_t live = g_live;
  const uint64_t untracked = g_untracked;
  mutexUnlock(&g_mutex);
  constexpr int64_t kScale = int64_t(kSampleMask) + 1;
  int64_t total = 0;
  uint32_t used = 0;
  for (uint32_t i = 0; i < kSites; ++i) {
    if (sites[i].pcs[0] != 0) {
      total += sites[i].bytes;
      ++used;
    }
  }
  const struct mallinfo heap = mallinfo();
  REXLOG_INFO("[heap_track] the heap: {:.1f} MB in use (mallinfo); allocator free {} bytes in {} chunks; "
              "releasable tail {} bytes; noted ~{:.1f} MB in ~{} live allocations (1 in "
              "16, times 16) from {} callers; {} not noted (table full)",
              double(heap.uordblks) / 1048576.0, uint64_t(heap.fordblks), uint64_t(heap.ordblks),
              uint64_t(heap.keepcost), double(total * kScale) / 1048576.0, uint64_t(live) * kScale, used,
              untracked);
  const uint64_t base = reinterpret_cast<uint64_t>(&_start);
  const auto line = [&](const char* what, uint32_t i) {
    const Site& s = sites[i];
    char pcs[kDepth][24];
    for (int d = 0; d < kDepth; ++d) {
      if (s.pcs[d] >= base && s.pcs[d] - base < 0x10000000) {
        std::snprintf(pcs[d], sizeof(pcs[d]), "image+%llx", (unsigned long long)(s.pcs[d] - base));
      } else {
        std::snprintf(pcs[d], sizeof(pcs[d]), "%llx", (unsigned long long)s.pcs[d]);
      }
    }
    REXLOG_INFO("[heap_track] {} {:+.2f} MB in the last minute, {:.2f} MB live in ~{} allocations (last one {} bytes) "
                "| {} <- {} <- {} <- {}",
                what, double((s.bytes - before[i]) * kScale) / 1048576.0, double(s.bytes * kScale) / 1048576.0,
                s.count * kScale, s.size, pcs[0], pcs[1], pcs[2], pcs[3]);
  };
  // The ten that grew most, then the five largest not among them.
  bool shown[kSites] = {};
  for (int n = 0; n < 10; ++n) {
    int64_t best = 0;
    uint32_t pick = kSites;
    for (uint32_t i = 0; i < kSites; ++i) {
      const int64_t grew = sites[i].bytes - before[i];
      if (sites[i].pcs[0] != 0 && !shown[i] && grew > best) {
        best = grew;
        pick = i;
      }
    }
    if (pick == kSites) {
      break;
    }
    shown[pick] = true;
    line("grew", pick);
  }
  for (int n = 0; n < 5; ++n) {
    int64_t best = 0;
    uint32_t pick = kSites;
    for (uint32_t i = 0; i < kSites; ++i) {
      if (sites[i].pcs[0] != 0 && !shown[i] && sites[i].bytes > best) {
        best = sites[i].bytes;
        pick = i;
      }
    }
    if (pick == kSites) {
      break;
    }
    shown[pick] = true;
    line("large", pick);
  }
  for (uint32_t i = 0; i < kSites; ++i) {
    before[i] = sites[i].bytes;
  }
}

#else

void bo2zm::heap_track::EveryFrame() {}

#endif  // BO2ZM_HEAP_TRACK
