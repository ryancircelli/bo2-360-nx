// Lost-wakeup stress tests for the POSIX waits: SignalAndWait ping-pong (rexglue PR 531) and Pulse / Set + Reset
// against a wait on two events, which the 1 ms poll of WaitMultiple used to miss.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/thread.h>

using namespace rex::thread;
using namespace std::chrono_literals;

namespace {
// Long enough that only a lost wakeup times out.
constexpr auto kStall = std::chrono::milliseconds(5000);
}  // namespace

TEST_CASE("SignalAndWait ping-pong loses no wakeup", "[thread][wait]") {
  constexpr int kRounds = 1000000;
  auto ping = Event::CreateAutoResetEvent(false);
  auto pong = Event::CreateAutoResetEvent(false);
  std::atomic<int> failures{0};
  std::thread other([&] {
    if (Wait(ping.get(), false, kStall) != WaitResult::kSuccess) {
      ++failures;
      return;
    }
    for (int i = 1; i < kRounds; ++i) {
      if (SignalAndWait(pong.get(), ping.get(), false, kStall) != WaitResult::kSuccess) {
        ++failures;
        return;
      }
    }
    pong->Set();
  });
  for (int i = 0; i < kRounds; ++i) {
    if (SignalAndWait(ping.get(), pong.get(), false, kStall) != WaitResult::kSuccess) {
      ++failures;
      break;
    }
  }
  other.join();
  REQUIRE(failures == 0);
}

// The waiter blocks in WaitAny on an idle event and a pulsed one (index 1, so the wake is not just the first slot);
// the other thread keeps pulsing until the waiter wakes. A pulse only reaches a thread that is already waiting, so
// each round passes only if the queued wait sees it. A late pulse may end the next round early, which is fine.
static void PulseRounds(bool manual_reset, bool set_reset, bool alertable) {
  constexpr int kRounds = 20000;
  std::unique_ptr<Event> events[2] = {
      manual_reset ? Event::CreateManualResetEvent(false) : Event::CreateAutoResetEvent(false),
      manual_reset ? Event::CreateManualResetEvent(false) : Event::CreateAutoResetEvent(false)};
  WaitHandle* handles[2] = {events[0].get(), events[1].get()};
  std::atomic<int> round{-1};  // The round the waiter is in, -1 before, kRounds when done.
  std::atomic<int> failures{0};
  WaitResult bad_result = WaitResult::kSuccess;
  size_t bad_index = 0;
  std::thread waiter([&] {
    for (int i = 0; i < kRounds; ++i) {
      round.store(i, std::memory_order_release);
      auto [result, index] = WaitAny(handles, 2, alertable, kStall);
      if (result != WaitResult::kSuccess || index != 1) {
        bad_result = result;
        bad_index = index;
        ++failures;
        break;
      }
    }
    round.store(kRounds, std::memory_order_release);
  });
  for (int i = 0; i < kRounds; ++i) {
    while (round.load(std::memory_order_acquire) < i) {
      std::this_thread::yield();
    }
    // Pulse until the waiter moves on (the first pulses may come before it is queued and are lost).
    while (round.load(std::memory_order_acquire) == i) {
      if (set_reset) {
        events[1]->Set();
        events[1]->Reset();
      } else {
        events[1]->Pulse();
      }
      std::this_thread::yield();
    }
    if (round.load(std::memory_order_acquire) == kRounds) {
      break;
    }
  }
  waiter.join();
  CAPTURE(manual_reset, set_reset, alertable, int(bad_result), bad_index);
  REQUIRE(failures == 0);
}

TEST_CASE("WaitAny on two events sees every Pulse", "[thread][wait]") {
  PulseRounds(false, false, false);
  PulseRounds(true, false, false);
}

TEST_CASE("WaitAny on two events sees Set + Reset", "[thread][wait]") {
  PulseRounds(false, true, false);
  PulseRounds(true, true, false);
}

TEST_CASE("Alertable WaitAny on two events sees every Pulse", "[thread][wait]") {
  PulseRounds(false, false, true);
}

// Two threads in WaitAny on the same semaphore and an idle event: every release is taken exactly once.
TEST_CASE("WaitAny takes each semaphore release once", "[thread][wait]") {
  constexpr int kReleases = 20000;
  auto semaphore = Semaphore::Create(0, kReleases);
  auto idle = Event::CreateManualResetEvent(false);
  WaitHandle* handles[2] = {semaphore.get(), idle.get()};
  std::atomic<int> taken{0};
  std::atomic<bool> stop{false};
  auto take = [&] {
    while (!stop.load(std::memory_order_acquire)) {
      auto [result, index] = WaitAny(handles, 2, false, 50ms);
      if (result == WaitResult::kSuccess && index == 0) {
        ++taken;
      }
    }
  };
  std::thread a(take);
  std::thread b(take);
  for (int i = 0; i < kReleases; ++i) {
    int previous = 0;
    semaphore->Release(1, &previous);
    if ((i & 63) == 0) {
      std::this_thread::yield();
    }
  }
  const auto give_up = std::chrono::steady_clock::now() + kStall;
  while (taken.load() < kReleases && std::chrono::steady_clock::now() < give_up) {
    std::this_thread::sleep_for(1ms);
  }
  std::this_thread::sleep_for(100ms);  // Room for a double take to show.
  stop = true;
  a.join();
  b.join();
  REQUIRE(taken == kReleases);
}
