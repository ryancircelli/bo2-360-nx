// Switch test build only: who calls memcpy (and memmove, memset, memcmp) on the native renderer's ring thread.
//
// Why: a stack-sampled profile puts ~8.5 % of the ring thread's busy time in memcpy, but its
// samples' lr is stale (the context Horizon hands back keeps the lr of the last system call), so the profile cannot
// name the callers.
//
// What: the linker routes the four functions to the wrappers below (-Wl,--wrap=..., app/CMakeLists.txt, only with
// BO2ZM_MEMCPY_CALLERS: BO2_MEMCPY_CALLERS=1 bash app/build_switch.sh). On the marked thread (the thread pointer of
// MarkThread's caller, read from tpidrro_el0, so no thread-local variable is touched before libnx sets them up), one
// call in 16 is noted by its return address with its bytes. Every 10 s Report logs the call sites with the most bytes
// and the most calls, times 16, as image offsets for tools/profile_sym.py. Calls GCC expands inline are not seen.

#include "bo2zm_memcpy_callers.h"

#if defined(BO2ZM_MEMCPY_CALLERS)

#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <rex/logging.h>

extern "C" {
void* __real_memcpy(void* d, const void* s, size_t n);
void* __real_memmove(void* d, const void* s, size_t n);
void* __real_memset(void* d, int c, size_t n);
int __real_memcmp(const void* a, const void* b, size_t n);
void _start(void);
}

namespace {

constexpr uint32_t kSampleMask = 15;  // one call in 16
constexpr uint32_t kSites = 1u << 12;
const char* const kNames[4] = {"memcpy", "memmove", "memset", "memcmp"};

struct Site {
  uintptr_t ra;  // 0 = empty
  uint32_t function;
  uint64_t calls;
  uint64_t bytes;
};
Site g_sites[kSites];  // written only by the marked thread
void* g_marked = nullptr;
uint32_t g_turn = 0;

inline void Note(uintptr_t ra, uint32_t function, size_t n) {
  if (!g_marked || armGetTls() != g_marked || (++g_turn & kSampleMask) != 0) {
    return;
  }
  uint32_t i = uint32_t((ra * 0x9E3779B97F4A7C15ull) >> 52) & (kSites - 1);
  for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSites - 1)) {
    Site& s = g_sites[i];
    if (s.ra == ra && s.function == function) {
      ++s.calls;
      s.bytes += n;
      return;
    }
    if (s.ra == 0) {
      s = {ra, function, 1, n};
      return;
    }
  }
}

}  // namespace

extern "C" {
__attribute__((used, noinline)) void* __wrap_memcpy(void* d, const void* s, size_t n) {
  Note(uintptr_t(__builtin_return_address(0)), 0, n);
  return __real_memcpy(d, s, n);
}
__attribute__((used, noinline)) void* __wrap_memmove(void* d, const void* s, size_t n) {
  Note(uintptr_t(__builtin_return_address(0)), 1, n);
  return __real_memmove(d, s, n);
}
__attribute__((used, noinline)) void* __wrap_memset(void* d, int c, size_t n) {
  Note(uintptr_t(__builtin_return_address(0)), 2, n);
  return __real_memset(d, c, n);
}
__attribute__((used, noinline)) int __wrap_memcmp(const void* a, const void* b, size_t n) {
  Note(uintptr_t(__builtin_return_address(0)), 3, n);
  return __real_memcmp(a, b, n);
}
}

void bo2zm::memcpy_callers::MarkThread() {
  g_marked = armGetTls();
  REXLOG_INFO("[memcpy_callers] counting this thread's memcpy, memmove, memset and memcmp calls by call site (1 in {})",
              kSampleMask + 1);
}

void bo2zm::memcpy_callers::Report() {
  static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  const auto now = std::chrono::steady_clock::now();
  if (armGetTls() != g_marked || now < next) {
    return;
  }
  next = now + std::chrono::seconds(10);
  std::vector<Site> sites;
  uint64_t calls = 0, bytes = 0;
  for (Site& s : g_sites) {
    if (s.ra) {
      sites.push_back(s);
      calls += s.calls;
      bytes += s.bytes;
    }
    s = {};
  }
  const uintptr_t base = uintptr_t(&_start);
  const auto line = [&](const Site& s) {
    char where[40];
    std::snprintf(where, sizeof(where), "image+0x%llx", (unsigned long long)(s.ra - base));
    return fmt::format(" {} {} x{} {:.1f} KB", kNames[s.function], where, s.calls * (kSampleMask + 1),
                       double(s.bytes) * (kSampleMask + 1) / 1024.0);
  };
  std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.bytes > b.bytes; });
  std::string by_bytes;
  for (size_t k = 0; k < sites.size() && k < 25; ++k) {
    by_bytes += line(sites[k]);
  }
  std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.calls > b.calls; });
  std::string by_calls;
  for (size_t k = 0; k < sites.size() && k < 25; ++k) {
    by_calls += line(sites[k]);
  }
  REXLOG_INFO("[memcpy_callers] last 10 s: ~{} calls, ~{:.1f} MB from {} call sites | by bytes:{} | by calls:{}",
              calls * (kSampleMask + 1), double(bytes) * (kSampleMask + 1) / 1048576.0, sites.size(), by_bytes,
              by_calls);
}

#else

void bo2zm::memcpy_callers::MarkThread() {}
void bo2zm::memcpy_callers::Report() {}

#endif
