// Deterministic benchmark pieces: the frame-locked guest clock (clock_frame_lock_hz) and spin accounting (SpinScope).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/thread.h>

#if defined(__APPLE__)
#include <pthread.h>
#endif

using rex::chrono::Clock;

TEST_CASE("frame-locked guest clock advances one step per frame", "[bench]") {
  // One binary, one setting: the lock reads its cvars once.
  REQUIRE(rex::cvar::SetFlagFromCommandLine("clock_frame_lock_hz", "100"));
  REQUIRE(rex::cvar::SetFlagFromCommandLine("clock_frame_lock_stall_ms", "1000"));
  Clock::set_guest_tick_frequency(50000000);
  REQUIRE(Clock::frame_locked());
  const uint64_t step = 50000000 / 100;
  Clock::OnGuestFrame();
  const uint64_t t0 = Clock::QueryGuestTickCount();
  // 30 ms of wall time inside one frame: guest time stops at the frame's step (10 ms).
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  REQUIRE(Clock::QueryGuestTickCount() - t0 <= step);
  // Each slow frame still covers exactly one step.
  uint64_t prev = Clock::QueryGuestTickCount();
  for (int i = 0; i < 5; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    Clock::OnGuestFrame();
    const uint64_t now = Clock::QueryGuestTickCount();
    REQUIRE(now >= prev);  // never goes back
    prev = now;
  }
  REQUIRE(prev - t0 >= 5 * step);
  REQUIRE(prev - t0 <= 6 * step + step / 10);  // the last frame's step, plus the time since it began
  REQUIRE(Clock::frame_lock_stalls() == 0);
  // A stall (no frame for longer than clock_frame_lock_stall_ms) lets guest time run with wall time again.
  const uint64_t before = Clock::QueryGuestTickCount();
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  REQUIRE(Clock::QueryGuestTickCount() - before > step + 50000000 / 10);
  Clock::OnGuestFrame();
  REQUIRE(Clock::frame_lock_stalls() == 1);
}

TEST_CASE("spin scopes count only the outermost scope, and only when enabled", "[bench]") {
  {
    rex::thread::SpinScope off;  // before EnableSpinCount: free, nothing recorded
  }
  rex::thread::EnableSpinCount();
  REQUIRE(rex::thread::SpinCountEnabled());
  {
    rex::thread::SpinScope outer;
    for (int i = 0; i < 100; ++i) rex::thread::MaybeYield();  // nested scopes inside MaybeYield
  }
  uint64_t total = 0;
  int threads = 0;
  std::pair<uint64_t*, int*> acc{&total, &threads};
  rex::thread::ForEachSpinCount(
      [](uint64_t, const rex::thread::SpinCounts& c, void* ctx) {
        auto* p = static_cast<std::pair<uint64_t*, int*>*>(ctx);
        *p->first += c.instr;
        ++*p->second;
      },
      &acc);
#if defined(__APPLE__)
  REQUIRE(threads == 1);
  REQUIRE(total > 100 * 500);  // a sched_yield is ~1700 instructions
#endif
}

TEST_CASE("gaps between spin scopes are binned by size", "[bench]") {
  rex::thread::EnableSpinCount();
  volatile uint64_t sink = 0;
  for (int i = 0; i < 50; ++i) {
    rex::thread::MaybeYield();
    for (int k = 0; k < 20000; ++k) sink = sink + k;  // a long gap: real work between two waits
  }
  rex::thread::MaybeYield();
  uint64_t n_long = 0, n_all = 0;
  std::pair<uint64_t*, uint64_t*> acc{&n_long, &n_all};
  rex::thread::ForEachSpinCount(
      [](uint64_t, const rex::thread::SpinCounts& c, void* ctx) {
        auto* p = static_cast<std::pair<uint64_t*, uint64_t*>*>(ctx);
        for (int b = 0; b < rex::thread::kSpinGapBuckets; ++b) {
          *p->second += c.gap_n[b];
          if (b >= 16) *p->first += c.gap_n[b];  // >= 32768 instructions
        }
      },
      &acc);
#if defined(__APPLE__)
  REQUIRE(n_long >= 50);
  REQUIRE(n_all >= n_long);
#endif
}

TEST_CASE("spin marks count a span once, less what it calls work, and a rewind drops them", "[bench]") {
  rex::thread::EnableSpinCount();
  const auto spin = [] {
    uint64_t total = 0;
    rex::thread::ForEachSpinCount(
        [](uint64_t tid, const rex::thread::SpinCounts& c, void* ctx) {
          uint64_t self = 0;
#if defined(__APPLE__)
          pthread_threadid_np(nullptr, &self);
#endif
          if (tid == self) *static_cast<uint64_t*>(ctx) += c.instr;
        },
        &total);
    return total;
  };
  volatile uint64_t sink = 0;
  const auto busy = [&] { for (int k = 0; k < 200000; ++k) sink = sink + k; };
  const uint64_t s0 = spin();
  auto mark = rex::thread::MarkSpin();
  busy();
  rex::thread::MaybeYield();  // a scope inside the span: counted by itself, not again by the span
  busy();
  const uint64_t inside = rex::thread::NotSpinSince(mark);
  rex::thread::CountSpinSince(mark, inside / 2);
  const uint64_t s1 = spin();
  mark = rex::thread::MarkSpin();
  rex::thread::SpinCountRewound();
  busy();
  rex::thread::CountSpinSince(mark);  // made before the rewind: dropped
  const uint64_t s2 = spin();
#if defined(__APPLE__)
  REQUIRE(inside > 2 * 200000);
  REQUIRE(s1 - s0 >= inside / 2);
  REQUIRE(s1 - s0 < inside);  // the yield scope plus half the span, not the span twice
  REQUIRE(s2 == s1);
#endif
}
