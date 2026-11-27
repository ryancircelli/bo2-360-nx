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
  std::atomic<uint64_t> gap_sum[kSpinGapBuckets]{}, gap_n[kSpinGapBuckets]{};
};
constexpr int kSlots = 1024;
Slot g_slots[kSlots];
std::atomic<int> g_used{0};
std::atomic<bool> g_enabled{false};
uint64_t g_overhead = 0;  // instructions of one counter read that land outside the bracket
std::atomic<uint32_t> g_epoch{1};  // bumped by a savepoint rewind
thread_local uint32_t t_epoch = 0;
thread_local Slot* t_slot = nullptr;
thread_local int t_depth = 0;  // only the outermost scope counts
thread_local uint64_t t_last_end = 0;  // counter at the end of this thread's last outermost scope, 0 = none yet

Slot* MySlot() {
  if (!t_slot) {
    const int i = g_used.fetch_add(1);
    if (i >= kSlots) return nullptr;  // shortcut: threads past 1024 are not counted; raise kSlots if a game has more
    t_slot = &g_slots[i];
#if defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    t_slot->tid.store(tid, std::memory_order_release);
#endif
  }
  return t_slot;
}

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
  epoch_ = g_epoch.load(std::memory_order_relaxed);
  if (t_epoch != epoch_) {  // first scope since a rewind: depth and last end belong to the restored stack
    t_epoch = epoch_;
    t_depth = 0;
    t_last_end = 0;
  }
  if (t_depth++ != 0) {
    start_ = 1;
    return;
  }
  start_ = SelfInstr();
  if (t_last_end && start_ > t_last_end) {
    if (Slot* slot = MySlot()) {
      const uint64_t gap = start_ - t_last_end;
      const int b = std::min(kSpinGapBuckets - 1, 64 - __builtin_clzll(gap));
      slot->gap_sum[b].fetch_add(gap, std::memory_order_relaxed);
      slot->gap_n[b].fetch_add(1, std::memory_order_relaxed);
    }
  }
}

SpinScope::~SpinScope() {
  if (!start_) return;
  if (epoch_ != g_epoch.load(std::memory_order_relaxed)) {  // made before a rewind, restored with the stack
    t_epoch = 0;
    return;
  }
  --t_depth;
  if (start_ == 1) return;
  const uint64_t end = SelfInstr();
  t_last_end = end;
  if (Slot* slot = MySlot()) slot->instr.fetch_add(end - start_ + g_overhead, std::memory_order_relaxed);
}

void SpinCountRewound() { g_epoch.fetch_add(1); }

SpinMark MarkSpin() {
  if (!g_enabled.load(std::memory_order_relaxed)) return {};
  const uint32_t e = g_epoch.load(std::memory_order_relaxed);
  Slot* slot = MySlot();
  if (!slot || (t_epoch == e && t_depth != 0)) return {};  // inside a scope: that scope counts it all
  return {SelfInstr(), slot->instr.load(std::memory_order_relaxed), e};
}

uint64_t NotSpinSince(const SpinMark& mark) {
  if (!mark.instr || mark.epoch != g_epoch.load(std::memory_order_relaxed)) return 0;
  const uint64_t span = SelfInstr() - mark.instr, inner = MySlot()->instr.load(std::memory_order_relaxed) - mark.spin;
  return span > inner ? span - inner : 0;
}

void CountSpinSince(const SpinMark& mark, uint64_t not_spin) {
  const uint64_t n = NotSpinSince(mark);
  if (n > not_spin) MySlot()->instr.fetch_add(n - not_spin, std::memory_order_relaxed);
}

void ForEachSpinCount(void (*fn)(uint64_t tid, const SpinCounts& counts, void* ctx), void* ctx) {
  const int n = std::min(g_used.load(), kSlots);
  for (int i = 0; i < n; ++i) {
    const uint64_t tid = g_slots[i].tid.load(std::memory_order_acquire);
    if (!tid) continue;
    SpinCounts c;
    c.instr = g_slots[i].instr.load(std::memory_order_relaxed);
    for (int b = 0; b < kSpinGapBuckets; ++b) {
      c.gap_sum[b] = g_slots[i].gap_sum[b].load(std::memory_order_relaxed);
      c.gap_n[b] = g_slots[i].gap_n[b].load(std::memory_order_relaxed);
    }
    fn(tid, c, ctx);
  }
}

}  // namespace rex::thread
