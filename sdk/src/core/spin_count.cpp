// Spin accounting for the deterministic benchmark (rex/thread.h SpinScope): instructions a thread spends in yield and
// spin loops grow with how long it waits, so they are counted apart and left out of the bench's work figure.
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#if defined(__APPLE__)
#include <pthread.h>
extern "C" int thread_selfcounts(int type, void* buf, size_t nbytes);  // libsystem_kernel; type 1 = {instr, cycles}
#endif

namespace rex::thread {
namespace {
struct Slot {
  std::atomic<uint64_t> tid{0};
  std::atomic<uint64_t> instr{0};
};
constexpr int kSlots = 1024;
Slot g_slots[kSlots];
std::atomic<int> g_used{0};
std::atomic<bool> g_enabled{false};
uint64_t g_overhead = 0;  // instructions of one counter read that land outside the bracket
thread_local Slot* t_slot = nullptr;
thread_local int t_depth = 0;  // only the outermost scope counts

uint64_t SelfInstr() {
#if defined(__APPLE__)
  uint64_t c[2] = {};
  thread_selfcounts(1, c, sizeof(c));
  return c[0];
#else
  return 0;
#endif
}
}  // namespace

void EnableSpinCount() {
#if defined(__APPLE__)
  uint64_t best = ~0ull;
  for (int i = 0; i < 32; ++i) {
    const uint64_t a = SelfInstr(), b = SelfInstr();
    best = std::min(best, b - a);
  }
  g_overhead = best;
  g_enabled.store(true, std::memory_order_release);
#endif
}

bool SpinCountEnabled() { return g_enabled.load(std::memory_order_relaxed); }

// start_: 0 = counting off, 1 = nested in another scope (only the outermost counts), else the counter at entry.
SpinScope::SpinScope() : start_(0) {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  start_ = t_depth++ == 0 ? SelfInstr() : 1;
}

SpinScope::~SpinScope() {
  if (!start_) return;
  --t_depth;
  if (start_ == 1) return;
  const uint64_t spent = SelfInstr() - start_ + g_overhead;
  if (!t_slot) {
    const int i = g_used.fetch_add(1);
    if (i >= kSlots) return;  // shortcut: threads past 1024 are not counted; raise kSlots if a game has more
    t_slot = &g_slots[i];
#if defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    t_slot->tid.store(tid, std::memory_order_release);
#endif
  }
  t_slot->instr.fetch_add(spent, std::memory_order_relaxed);
}

void ForEachSpinCount(void (*fn)(uint64_t tid, uint64_t instr, void* ctx), void* ctx) {
  const int n = std::min(g_used.load(), kSlots);
  for (int i = 0; i < n; ++i) {
    const uint64_t tid = g_slots[i].tid.load(std::memory_order_acquire);
    if (tid) fn(tid, g_slots[i].instr.load(std::memory_order_relaxed), ctx);
  }
}

}  // namespace rex::thread
