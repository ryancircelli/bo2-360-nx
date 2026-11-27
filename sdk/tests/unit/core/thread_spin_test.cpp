// SPDX-License-Identifier: 0BSD
// Spin-wait detector state machine (no clocks, no sleeps), then GuestPoll end to end.
#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <rex/cvar.h>
#include <rex/thread.h>
#include <rex/thread_spin.h>

using rex::thread::SpinTracker;

namespace {
SpinTracker::Config Cfg() {
  SpinTracker::Config c;
  c.gap_ns = 200000;
  c.threshold = 4;
  c.step_us = 10;
  c.max_sleep_us = 50;
  return c;
}
}  // namespace

TEST_CASE("Back to back polls start sleeping after the threshold and grow to the cap", "[thread_spin]") {
  SpinTracker t;
  const auto c = Cfg();
  uint64_t now = 1000000;
  uint32_t sleeps[10];
  for (int i = 0; i < 10; ++i) {
    sleeps[i] = t.OnPoll(now, c);
    now += 2000 + uint64_t(sleeps[i]) * 1000;  // 2 us of other work, plus our own sleep
    t.OnReturn(now);
  }
  CHECK(sleeps[0] == 0);
  CHECK(sleeps[3] == 0);  // 4th poll of the spin: still at the threshold
  CHECK(sleeps[4] == 10);
  CHECK(sleeps[5] == 20);
  CHECK(sleeps[8] == 50);
  CHECK(sleeps[9] == 50);  // capped
}

TEST_CASE("A gap of other work ends the spin", "[thread_spin]") {
  SpinTracker t;
  const auto c = Cfg();
  uint64_t now = 1000000;
  for (int i = 0; i < 8; ++i) {
    t.OnPoll(now, c);
    now += 1000;
    t.OnReturn(now);
  }
  CHECK(t.run == 8);
  now += 500000;  // 500 us of real work
  CHECK(t.OnPoll(now, c) == 0);
  CHECK(t.run == 1);
}

TEST_CASE("Polls separated by real work never back off", "[thread_spin]") {
  SpinTracker t;
  const auto c = Cfg();
  uint64_t now = 1000000;
  for (int i = 0; i < 100; ++i) {
    CHECK(t.OnPoll(now, c) == 0);
    t.OnReturn(now);
    now += 300000;  // real work before the next poll
  }
}

TEST_CASE("The first poll of a thread and a clock that steps back are not spins", "[thread_spin]") {
  SpinTracker t;
  const auto c = Cfg();
  CHECK(t.OnPoll(5000, c) == 0);
  t.OnReturn(5000);
  CHECK(t.OnPoll(4000, c) == 0);
  CHECK(t.run == 1);
}

// GuestPoll with every combination of the bench knobs: the knobs change only how a poll yields and sleeps, never
// which polls back off or for how long. The gap is set to 100 ms so a preempted test thread cannot end the spin.
namespace {
rex::thread::SpinStats Delta(const rex::thread::SpinStats& a, const rex::thread::SpinStats& b) {
  return {b.polls - a.polls, b.backoffs - a.backoffs, b.slept_us - a.slept_us};
}
}  // namespace

TEST_CASE("GuestPoll backs off the same with every sleep knob", "[thread_spin]") {
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_backoff", "true"));
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_threshold", "4"));
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_gap_us", "100000"));
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_step_us", "10"));
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_max_us", "50"));
  for (const char* without_yield : {"false", "true"}) {
    for (const char* coarse : {"false", "true"}) {
      REQUIRE(rex::cvar::SetFlagByName("spin_wait_sleep_without_yield", without_yield));
      REQUIRE(rex::cvar::SetFlagByName("spin_wait_coarse_sleep", coarse));
      rex::thread::Sleep(std::chrono::milliseconds(150));  // a gap: the next poll starts a new spin
      const auto before = rex::thread::GetSpinStats();
      for (int i = 0; i < 20; ++i) rex::thread::GuestPoll();
      const auto d = Delta(before, rex::thread::GetSpinStats());
      CHECK(d.polls == 20);
      CHECK(d.backoffs == 16);                 // polls 5..20
      CHECK(d.slept_us == 10 + 20 + 30 + 40 + 50 * 12);
    }
  }
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_backoff", "false"));
  const auto before = rex::thread::GetSpinStats();
  for (int i = 0; i < 20; ++i) rex::thread::GuestPoll();  // off: a plain yield, nothing counted
  CHECK(Delta(before, rex::thread::GetSpinStats()).polls == 0);
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_sleep_without_yield", "false"));
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_coarse_sleep", "false"));
  REQUIRE(rex::cvar::SetFlagByName("spin_wait_backoff", "true"));
}
