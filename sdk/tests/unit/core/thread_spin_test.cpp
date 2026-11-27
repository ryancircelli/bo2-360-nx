// SPDX-License-Identifier: 0BSD
// Spin-wait detector state machine (no clocks, no sleeps).
#include <catch2/catch_test_macros.hpp>

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
