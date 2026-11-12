#include "bo2zm_critpath.h"
#include <rex/cvar.h>
#include <rex/graphics/swap_hook.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

REXCVAR_DEFINE_BOOL(bo2zm_critpath, false, "BO2", "Record fence and Main group-wait critical paths")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(std::string, bo2zm_perf_log);

namespace bo2zm::critpath {
namespace {
using Clock = std::chrono::steady_clock;
uint64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
constexpr size_t kFences = 1 << 20, kWaits = 1 << 16, kFrames = 1 << 14;
// Never recycle a slot while producers may hold it. Saturation is reported instead of silently aliasing fences.
struct Fence {
  std::atomic<uint32_t> value{0}, address{0};
  std::atomic<uint64_t> issue{0}, exec{0}, pending{0}, pass{0}, count{0};
};
std::array<Fence, kFences> fences;
const uint64_t epoch = Now();
std::atomic<uint64_t> collisions{0}, latest_pending{0};
Fence* Find(uint32_t value) {
  if (!value) return nullptr;
  auto& f = fences[(value >> 1) & (kFences - 1)];
  uint32_t expected = f.value.load(std::memory_order_relaxed);
  if (expected == value) return &f;
  if (expected != 0 || !f.value.compare_exchange_strong(expected, value, std::memory_order_relaxed)) {
    if (expected == value) return &f;
    collisions.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  return &f;
}
void First(std::atomic<uint64_t>& dst, uint64_t ns) {
  auto before = dst.load(std::memory_order_relaxed);
  while ((before == 0 || ns < before) &&
         !dst.compare_exchange_weak(before, ns, std::memory_order_relaxed)) {}
}
struct Wait { uint64_t start = 0, end = 0; uint32_t fence = 0; };
std::array<Wait, kWaits> waits;
// Main is the only wait producer; CP is the only consumer. A short sequence covers metadata publication,
// never the guest wait. All fields read across threads are atomic; ring payloads use release/acquire indices.
std::atomic<uint64_t> wait_seq{0}, wait_write{0}, wait_read{0}, active_start{0}, wait_drops{0}, wait_calls{0};
thread_local unsigned wait_depth = 0;
struct Frame {
  uint64_t frame = 0, present = 0, total = 0, calls = 0, start = 0, end = 0;
  uint32_t fence = 0, ongoing = 0;
};
std::array<Frame, kFrames> frames;
std::atomic<uint64_t> frame_write{0}, frame_read{0}, frame_drops{0};
uint64_t previous_present = 0, previous_calls = 0;
std::atomic<bool> stop{false};
std::thread writer;
uint32_t LatestDuring(uint64_t start, uint64_t end) {
  // High word is process-relative microseconds (valid for runs shorter than 71 minutes).
  const uint64_t packed = latest_pending.load(std::memory_order_relaxed);
  const uint64_t us = packed >> 32;
  return us >= (start - epoch + 999) / 1000 && us <= (end - epoch) / 1000 ? uint32_t(packed) : 0;
}
void Executed(uint32_t address, uint32_t value) {
  // CP gives the logical packet value before the endian transform, after its store has completed.
  auto& f = fences[(value >> 1) & (kFences - 1)];
  if (value && f.value.load(std::memory_order_relaxed) == value &&
      f.address.load(std::memory_order_acquire) == address) First(f.exec, Now());
}
void Writer(std::filesystem::path dir) {
  rex::thread::set_current_thread_name("BO2 critical path");
  auto* ff = std::fopen((dir / "critpath_fences.tsv").c_str(), "wb");
  auto* fr = std::fopen((dir / "critpath_frames.tsv").c_str(), "wb");
  if (!ff || !fr) { if (ff) std::fclose(ff); if (fr) std::fclose(fr); return; }
  std::fprintf(ff, "# monotonic ns; append-only snapshots: use LAST row per (address,value); zero=unobserved\nvalue\taddress\tt_issue\tt_exec\tt_first_pending\tt_pass\tpending_calls\n");
  std::fprintf(fr, "# monotonic ns; wait_ns is interval overlap with this frame; calls counts all invocations; longest is full wait span (clipped at present when ongoing); fence=latest pending seen by any thread during wait\nframe\tt_present\tmain_wait_ns\tmain_wait_calls\tlongest_start\tlongest_end\tfence\tongoing\twait_drops\tframe_drops\tfence_collisions\n");
  // The reporter alone owns these snapshots. No producer allocates or takes a lock.
  struct Snapshot { uint64_t issue=0, exec=0, pending=0, pass=0, count=0; };
  auto last = std::make_unique<Snapshot[]>(kFences);
  for (;;) {
    bool done = stop.load(std::memory_order_acquire);
    for (size_t i = 0; i < kFences; ++i) {
      auto& f = fences[i];
      auto v = f.value.load(std::memory_order_relaxed);
      if (!v) continue;
      Snapshot s{f.issue.load(), f.exec.load(), f.pending.load(), f.pass.load(), f.count.load()};
      auto& l = last[i];
      if (s.issue==l.issue && s.exec==l.exec && s.pending==l.pending && s.pass==l.pass && s.count==l.count) continue;
      std::fprintf(ff, "%u\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\n", v, f.address.load(), (unsigned long long)s.issue, (unsigned long long)s.exec, (unsigned long long)s.pending, (unsigned long long)s.pass, (unsigned long long)s.count);
      l = s;
    }
    auto r = frame_read.load(std::memory_order_relaxed), w = frame_write.load(std::memory_order_acquire);
    for (; r < w; ++r) {
      const auto& f = frames[r & (kFrames-1)];
      std::fprintf(fr, "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%llu\t%llu\t%llu\n", (unsigned long long)f.frame, (unsigned long long)f.present, (unsigned long long)f.total, (unsigned long long)f.calls, (unsigned long long)f.start, (unsigned long long)f.end, f.fence, f.ongoing, (unsigned long long)wait_drops.load(), (unsigned long long)frame_drops.load(), (unsigned long long)collisions.load());
    }
    frame_read.store(r, std::memory_order_release);
    std::fflush(ff); std::fflush(fr);
    if (done) break;
    // Short sleeps permit fast orderly shutdown without a hot-path notification.
    for (int i=0; i<10 && !stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::fclose(ff); std::fclose(fr);
}
}
bool Enabled() {
  static const bool on = [] {
    const char* env = std::getenv("REX_BO2ZM_CRITPATH");
    return (env && std::string_view(env) == "1") || REXCVAR_GET(bo2zm_critpath);
  }();
  return on;
}
void Install() {
  if (!Enabled() || writer.joinable()) return;
  auto path = std::filesystem::path(REXCVAR_GET(bo2zm_perf_log));
  writer = std::thread(Writer, path.empty() ? std::filesystem::current_path() : path.parent_path());
  rex::graphics::SetGuestFenceWriteHook(Executed);
}
void Issue(uint32_t value, uint32_t guest_address) {
  if (auto* f = Find(value)) {
    const uint32_t physical = (guest_address & 0x1FFFFFFF) + (((guest_address >> 20) + 512) & 0x1000);
    f->address.store(physical, std::memory_order_release);
    First(f->issue, Now());
  }
}
void Test(uint32_t value, bool pending) {
  auto* f = Find(value);
  if (!f) return;
  if (pending) {
    const auto ns = Now();
    First(f->pending, ns);
    f->count.fetch_add(1, std::memory_order_relaxed);
    const uint64_t packed = (((ns - epoch) / 1000) << 32) | value;
    auto before = latest_pending.load(std::memory_order_relaxed);
    while (before < packed && !latest_pending.compare_exchange_weak(before, packed, std::memory_order_relaxed)) {}
  } else if (!f->pass.load(std::memory_order_relaxed)) First(f->pass, Now());
}
bool IsMain() {
  static thread_local const bool main = [] {
    auto* x = rex::system::XThread::GetCurrentThread();
    auto* t = x && x->thread() ? x->thread() : rex::thread::Thread::GetCurrentThread();
    return t && t->name().starts_with("Main");
  }();
  return main;
}
void BeginWait() {
  wait_seq.fetch_add(1);
  wait_calls.fetch_add(1);
  if (wait_depth++ == 0) active_start.store(Now());  // nested waits count once in wall time
  wait_seq.fetch_add(1);
}
void EndWait() {
  if (--wait_depth) return;
  const uint64_t end = Now(), start = active_start.load();
  wait_seq.fetch_add(1);
  auto w = wait_write.load(std::memory_order_relaxed);
  if (w - wait_read.load(std::memory_order_acquire) < kWaits) {
    waits[w & (kWaits-1)] = {start, end, LatestDuring(start, end)};
    wait_write.store(w+1, std::memory_order_release);
  } else wait_drops.fetch_add(1);
  active_start.store(0);
  wait_seq.fetch_add(1);
}
void Present(uint64_t frame) {
  if (!Enabled()) return;
  uint64_t seq, w, start, present, calls;
  do {
    seq = wait_seq.load();
    if (seq & 1) continue;
    w = wait_write.load(); start = active_start.load(); calls = wait_calls.load(); present = Now();
  } while ((seq & 1) || seq != wait_seq.load());
  Frame f; f.frame = frame; f.present = present; f.calls = calls - previous_calls;
  previous_calls = calls;
  auto add = [&](const Wait& a, bool ongoing) {
    if (a.end <= previous_present || a.start > present) return;
    f.total += std::min(a.end, present) - std::max(a.start, previous_present);
    if (a.end - a.start > f.end - f.start) {
      f.start = a.start; f.end = a.end; f.fence = a.fence; f.ongoing = ongoing;
    }
  };
  auto r = wait_read.load(std::memory_order_relaxed);
  for (; r < w; ++r) add(waits[r & (kWaits-1)], false);
  wait_read.store(r, std::memory_order_release);
  if (start) add({start, present, LatestDuring(start, present)}, true);
  previous_present = present;
  auto fw = frame_write.load(std::memory_order_relaxed);
  if (fw - frame_read.load(std::memory_order_acquire) < kFrames) {
    frames[fw & (kFrames-1)] = f;
    frame_write.store(fw+1, std::memory_order_release);
  } else frame_drops.fetch_add(1);
}
void Close() {
  if (!Enabled()) return;
  rex::graphics::SetGuestFenceWriteHook(nullptr);
  stop.store(true, std::memory_order_release);
  if (writer.joinable()) writer.join();
}
}
