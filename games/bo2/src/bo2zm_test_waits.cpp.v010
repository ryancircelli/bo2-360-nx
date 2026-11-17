// See bo2zm_test_waits.h.

#include "bo2zm_test_waits.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/thread.h>

#if !defined(BO2ZM_TEST_WAITS_STANDALONE)
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>

REXCVAR_DEFINE_BOOL(bo2zm_test_waits, false, "BO2",
                    "Test helper: stress test of the runtime's waits and signals at start-up, PASS/FAIL lines "
                    "'[test_waits]' in the log (a few seconds)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

#if defined(__SWITCH__) || defined(REX_FAST_WAITS_TEST)
namespace rex::thread {
// threading_posix.cpp (REX_FAST_WAITS)
uint64_t FastWaitLostWakeups();
uint32_t FastWaitWaiters(WaitHandle* handle);
}  // namespace rex::thread
#define BO2ZM_FAST_WAITS_HOOKS 1
#else
#define BO2ZM_FAST_WAITS_HOOKS 0
#endif

namespace bo2zm::test_waits {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using rex::thread::Event;
using rex::thread::Mutant;
using rex::thread::Semaphore;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;

// Every wait that should end has this much time to do it. A lost wake-up on the REX_FAST_WAITS path costs at most
// ~1 s (its check interval), so it still passes the wait but is counted by the runtime and fails the summary.
constexpr auto kPatience = 3000ms;

std::string Format(const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return buffer;
}

double Ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

int Scaled(double scale, int n) {
  return std::max(1, int(n * scale));
}

// Polls pred every 50 us for at most limit.
template <typename Pred>
bool WaitUntil(Pred pred, Clock::duration limit) {
  const auto end = Clock::now() + limit;
  while (!pred()) {
    if (Clock::now() > end) {
      return false;
    }
    std::this_thread::sleep_for(50us);
  }
  return true;
}

// Until count threads are inside a wait on handle, so a pulse cannot miss one that is still on its way in (a
// pulse rightly releases only the threads already waiting). The runtime tells on REX_FAST_WAITS; elsewhere a
// pause has to do.
bool UntilWaiting(WaitHandle* handle, uint32_t count) {
#if BO2ZM_FAST_WAITS_HOOKS
  return WaitUntil([&] { return rex::thread::FastWaitWaiters(handle) >= count; }, kPatience);
#else
  (void)handle;
  (void)count;
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return true;
#endif
}

WaitResult Wait(WaitHandle* handle, std::chrono::milliseconds timeout, bool alertable = false) {
  return rex::thread::Wait(handle, alertable, timeout);
}

WaitResult WaitAll(std::vector<WaitHandle*> handles, std::chrono::milliseconds timeout) {
  return rex::thread::WaitAll(std::move(handles), false, timeout);
}

std::pair<WaitResult, size_t> WaitAny(std::vector<WaitHandle*> handles, std::chrono::milliseconds timeout,
                                      bool alertable = false) {
  return rex::thread::WaitAny(std::move(handles), alertable, timeout);
}

// Each test returns "" when it passes, else what went wrong; *detail gets numbers worth logging either way.
using Test = std::string (*)(double scale, std::string* detail);

// Two threads hand an auto-reset event back and forth.
std::string PingPong(double scale, std::string* detail) {
  const int rounds = Scaled(scale, 20000);
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  std::atomic<int> failed{0};
  std::thread other([&] {
    for (int i = 0; i < rounds; ++i) {
      if (Wait(a.get(), kPatience) != WaitResult::kSuccess) {
        ++failed;
        return;
      }
      b->Set();
    }
  });
  const auto start = Clock::now();
  int done = 0;
  for (; done < rounds; ++done) {
    a->Set();
    if (Wait(b.get(), kPatience) != WaitResult::kSuccess) {
      ++failed;
      break;
    }
  }
  const auto elapsed = Clock::now() - start;
  other.join();
  *detail = Format("%d round trips, %.2f us each", done, Ms(elapsed) * 1000.0 / std::max(1, done));
  return failed ? Format("a wait timed out (%d)", failed.load()) : "";
}

// An auto-reset event releases exactly one waiter per Set, and is not left signaled.
std::string AutoOnePerSet(double scale, std::string* detail) {
  const int sets = Scaled(scale, 2000);
  constexpr int kWaiters = 4;
  auto ev = Event::CreateAutoResetEvent(false);
  std::atomic<int> released{0}, timeouts{0}, exited{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> waiters;
  for (int k = 0; k < kWaiters; ++k) {
    waiters.emplace_back([&] {
      for (;;) {
        const WaitResult r = Wait(ev.get(), kPatience);
        if (stop) {
          break;
        }
        if (r == WaitResult::kSuccess) {
          ++released;
        } else {
          ++timeouts;
        }
      }
      ++exited;
    });
  }
  std::string error;
  double worst = 0, total = 0;
  for (int i = 0; i < sets && error.empty(); ++i) {
    const auto t0 = Clock::now();
    ev->Set();
    if (!WaitUntil([&] { return released.load() >= i + 1; }, kPatience)) {
      error = Format("Set %d released nobody", i);
      break;
    }
    const double latency = Ms(Clock::now() - t0);
    total += latency;
    worst = std::max(worst, latency);
  }
  std::this_thread::sleep_for(5ms);
  const int extra = released.load() - sets;
  if (error.empty() && extra != 0) {
    error = Format("%d more releases than Sets", extra);
  }
  if (error.empty() && Wait(ev.get(), 0ms) == WaitResult::kSuccess) {
    error = "the event was left signaled";
  }
  if (error.empty() && timeouts) {
    error = Format("%d waits timed out", timeouts.load());
  }
  stop = true;
  WaitUntil(
      [&] {
        ev->Set();
        return exited.load() == kWaiters;
      },
      2 * kPatience);
  for (auto& t : waiters) {
    t.join();
  }
  *detail = Format("%d Sets, release after %.3f ms on average, worst %.3f ms", sets, total / sets, worst);
  return error;
}

// A manual-reset event releases every thread waiting on it. reset_at_once: Reset right after Set, which must still
// release them (a waiter woken by Set must not find the event reset and go back to sleep).
std::string ManualReleasesAll(double scale, std::string* detail, int mode) {
  // mode 0: Set, Reset once all are out. 1: Set then Reset at once. 2: PulseEvent.
  const int rounds = Scaled(scale, 300);
  constexpr int kWaiters = 6;
  auto ev = Event::CreateManualResetEvent(false);
  std::atomic<int> arrived{0}, released{0}, timeouts{0}, round{-1};
  std::atomic<bool> stop{false};
  std::vector<std::thread> waiters;
  for (int k = 0; k < kWaiters; ++k) {
    waiters.emplace_back([&] {
      for (int r = 0;; ++r) {
        WaitUntil([&] { return round.load() >= r || stop.load(); }, 60s);
        if (stop) {
          return;
        }
        ++arrived;
        if (Wait(ev.get(), kPatience) == WaitResult::kSuccess) {
          ++released;
        } else {
          ++timeouts;
        }
      }
    });
  }
  std::string error;
  for (int r = 0; r < rounds && error.empty(); ++r) {
    round = r;
    if (!WaitUntil([&] { return arrived.load() == kWaiters * (r + 1); }, kPatience) ||
        !UntilWaiting(ev.get(), kWaiters)) {
      error = "waiters did not arrive";
      break;
    }
    if (mode == 2) {
      ev->Pulse();
    } else {
      ev->Set();
      if (mode == 1) {
        ev->Reset();
      }
    }
    if (!WaitUntil([&] { return released.load() + timeouts.load() == kWaiters * (r + 1); }, 2 * kPatience)) {
      error = Format("round %d: waiters still inside", r);
      break;
    }
    if (mode == 0) {
      ev->Reset();
    }
    if (mode == 2 && Wait(ev.get(), 0ms) == WaitResult::kSuccess) {
      error = Format("round %d: the pulse left the event signaled", r);
    }
  }
  stop = true;
  for (auto& t : waiters) {
    t.join();
  }
  if (error.empty() && timeouts) {
    error = Format("%d of %d waiters were not released", timeouts.load(), kWaiters * rounds);
  }
  *detail = Format("%d rounds of %d waiters", rounds, kWaiters);
  return error;
}

std::string ManualSet(double scale, std::string* detail) {
  return ManualReleasesAll(scale, detail, 0);
}
std::string ManualSetReset(double scale, std::string* detail) {
  return ManualReleasesAll(scale, detail, 1);
}
std::string ManualPulse(double scale, std::string* detail) {
  return ManualReleasesAll(scale, detail, 2);
}

// PulseEvent on an auto-reset event releases one waiter per pulse and leaves it non-signaled.
std::string AutoPulse(double scale, std::string* detail) {
  const int rounds = Scaled(scale, 200);
  constexpr int kWaiters = 4;
  auto ev = Event::CreateAutoResetEvent(false);
  std::atomic<int> arrived{0}, released{0}, timeouts{0}, round{-1};
  std::atomic<bool> stop{false};
  std::vector<std::thread> waiters;
  for (int k = 0; k < kWaiters; ++k) {
    waiters.emplace_back([&] {
      for (int r = 0;; ++r) {
        WaitUntil([&] { return round.load() >= r || stop.load(); }, 60s);
        if (stop) {
          return;
        }
        ++arrived;
        if (Wait(ev.get(), kPatience) == WaitResult::kSuccess) {
          ++released;
        } else {
          ++timeouts;
        }
      }
    });
  }
  std::string error;
  for (int r = 0; r < rounds && error.empty(); ++r) {
    round = r;
    if (!WaitUntil([&] { return arrived.load() == kWaiters * (r + 1); }, kPatience) ||
        !UntilWaiting(ev.get(), kWaiters)) {
      error = "waiters did not arrive";
      break;
    }
    for (int k = 0; k < kWaiters && error.empty(); ++k) {
      const int expected = kWaiters * r + k + 1;
      ev->Pulse();
      if (!WaitUntil([&] { return released.load() + timeouts.load() >= expected; }, 2 * kPatience)) {
        error = Format("round %d, pulse %d released nobody", r, k);
        break;
      }
      std::this_thread::sleep_for(200us);
      if (released.load() + timeouts.load() > expected) {
        error = Format("round %d, pulse %d released more than one waiter", r, k);
      }
    }
    if (error.empty() && Wait(ev.get(), 0ms) == WaitResult::kSuccess) {
      error = Format("round %d: the event was left signaled", r);
    }
  }
  stop = true;
  for (auto& t : waiters) {
    t.join();
  }
  if (error.empty() && timeouts) {
    error = Format("%d waits timed out", timeouts.load());
  }
  *detail = Format("%d rounds of %d pulses", rounds, kWaiters);
  return error;
}

// Single-thread rules: a pulse with nobody waiting resets the event; Set on a signaled event changes nothing;
// semaphore counts and limits; zero timeouts.
std::string Rules(double, std::string* detail) {
  for (bool manual : {true, false}) {
    auto ev = manual ? Event::CreateManualResetEvent(true) : Event::CreateAutoResetEvent(true);
    ev->Pulse();
    if (Wait(ev.get(), 0ms) == WaitResult::kSuccess) {
      return Format("%s: a pulse with nobody waiting left the event signaled", manual ? "manual" : "auto");
    }
    ev->Set();
    ev->Set();
    if (Wait(ev.get(), 0ms) != WaitResult::kSuccess) {
      return "Set did not signal";
    }
    const bool still = Wait(ev.get(), 0ms) == WaitResult::kSuccess;
    if (still != manual) {
      return manual ? "a manual-reset event reset itself" : "an auto-reset event released twice";
    }
    ev->Reset();
    if (Wait(ev.get(), 0ms) == WaitResult::kSuccess) {
      return "Reset did not reset";
    }
  }
  auto sem = Semaphore::Create(2, 5);
  int previous = -1;
  if (!sem->Release(2, &previous) || previous != 2) {
    return Format("Release(2) from 2: previous %d", previous);
  }
  if (sem->Release(2, &previous)) {
    return "Release past the maximum succeeded";
  }
  if (!sem->Release(1, &previous) || previous != 4) {
    return Format("Release(1) from 4: previous %d", previous);
  }
  for (int i = 0; i < 5; ++i) {
    if (Wait(sem.get(), 0ms) != WaitResult::kSuccess) {
      return Format("semaphore: only %d of 5 taken", i);
    }
  }
  if (Wait(sem.get(), 0ms) == WaitResult::kSuccess) {
    return "semaphore: took a sixth";
  }
  if (Semaphore::Create(3, 2) != nullptr) {
    return "semaphore created with initial > maximum";
  }
  *detail = "events, pulses, semaphore counts";
  return "";
}

// Producers release a semaphore, consumers take every unit exactly once.
std::string SemaphoreCounts(double scale, std::string* detail) {
  constexpr int kProducers = 3, kConsumers = 4;
  const int per_producer = Scaled(scale, 5000);
  const int total = kProducers * per_producer;
  auto sem = Semaphore::Create(0, 64);
  std::atomic<int> taken{0}, timeouts{0}, full{0};
  std::vector<std::thread> threads;
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&] {
      for (;;) {
        if (Wait(sem.get(), kPatience) != WaitResult::kSuccess) {
          ++timeouts;
          return;
        }
        if (++taken > total) {
          return;  // a stop token
        }
      }
    });
  }
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back([&] {
      for (int i = 0; i < per_producer; ++i) {
        while (!sem->Release(1, nullptr)) {
          ++full;
          std::this_thread::sleep_for(20us);
        }
      }
    });
  }
  std::string error;
  if (!WaitUntil([&] { return taken.load() >= total; }, 30s)) {
    error = Format("only %d of %d units taken", taken.load(), total);
  }
  sem->Release(kConsumers, nullptr);  // one stop token each
  for (auto& t : threads) {
    t.join();
  }
  if (error.empty() && taken.load() != total + kConsumers) {
    error = Format("%d units taken for %d released", taken.load(), total + kConsumers);
  }
  if (error.empty() && Wait(sem.get(), 0ms) == WaitResult::kSuccess) {
    error = "a unit was left over";
  }
  if (error.empty() && timeouts) {
    error = Format("%d waits timed out", timeouts.load());
  }
  *detail = Format("%d units, %d times full", total, full.load());
  return error;
}

// WaitAny over two semaphores and an auto-reset event: every signal is taken once, by index.
std::string WaitAnyCounts(double scale, std::string* detail) {
  constexpr int kConsumers = 4;
  const int n = Scaled(scale, 3000);
  auto a = Semaphore::Create(0, 1000);
  auto b = Semaphore::Create(0, 1000);
  auto c = Event::CreateAutoResetEvent(false);
  std::atomic<int> got[3] = {{0}, {0}, {0}};
  std::atomic<int> timeouts{0}, wrong{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int k = 0; k < kConsumers; ++k) {
    threads.emplace_back([&] {
      for (;;) {
        const auto r = WaitAny({a.get(), b.get(), c.get()}, kPatience);
        if (r.first != WaitResult::kSuccess) {
          ++timeouts;
          return;
        }
        if (r.second > 2) {
          ++wrong;
          return;
        }
        ++got[r.second];
        if (stop && r.second == 0) {
          return;
        }
      }
    });
  }
  threads.emplace_back([&] {
    for (int i = 0; i < n; ++i) {
      while (!a->Release(1, nullptr)) {
        std::this_thread::sleep_for(20us);
      }
    }
  });
  threads.emplace_back([&] {
    for (int i = 0; i < n; ++i) {
      while (!b->Release(1, nullptr)) {
        std::this_thread::sleep_for(20us);
      }
    }
  });
  std::string error;
  for (int i = 0; i < n && error.empty(); ++i) {
    c->Set();
    if (!WaitUntil([&] { return got[2].load() > i; }, kPatience)) {
      error = Format("event Set %d was not taken", i);
    }
  }
  if (error.empty() &&
      !WaitUntil([&] { return got[0].load() >= n && got[1].load() >= n; }, 30s)) {
    error = Format("taken %d/%d/%d of %d each", got[0].load(), got[1].load(), got[2].load(), n);
  }
  stop = true;
  a->Release(kConsumers, nullptr);
  for (auto& t : threads) {
    t.join();
  }
  if (error.empty() && (got[0].load() != n + kConsumers || got[1].load() != n || got[2].load() != n)) {
    error = Format("taken %d/%d/%d, expected %d/%d/%d", got[0].load(), got[1].load(), got[2].load(),
                   n + kConsumers, n, n);
  }
  if (error.empty() && (wrong || timeouts)) {
    error = Format("%d bad indices, %d timeouts", wrong.load(), timeouts.load());
  }
  *detail = Format("%d signals on each of 3 objects, %d threads", n, kConsumers);
  return error;
}

// WaitAll takes one unit of each semaphore at once, never one without the other.
std::string WaitAllCounts(double scale, std::string* detail) {
  constexpr int kConsumers = 3;
  const int n = Scaled(scale, 3000);
  auto a = Semaphore::Create(0, 100000);
  auto b = Semaphore::Create(0, 100000);
  std::atomic<int> done{0}, timeouts{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int k = 0; k < kConsumers; ++k) {
    threads.emplace_back([&] {
      for (;;) {
        if (WaitAll({a.get(), b.get()}, kPatience) != WaitResult::kSuccess) {
          ++timeouts;
          return;
        }
        ++done;
        if (stop) {
          return;
        }
      }
    });
  }
  for (auto* sem : {a.get(), b.get()}) {
    threads.emplace_back([&, sem] {
      std::minstd_rand rng(sem == a.get() ? 1 : 2);
      for (int i = 0; i < n; ++i) {
        sem->Release(1, nullptr);
        if (rng() % 8 == 0) {
          std::this_thread::sleep_for(10us);
        }
      }
    });
  }
  std::string error;
  if (!WaitUntil([&] { return done.load() >= n; }, 30s)) {
    error = Format("only %d of %d pairs taken", done.load(), n);
  }
  std::this_thread::sleep_for(5ms);
  if (error.empty() && done.load() != n) {
    error = Format("%d pairs taken from %d", done.load(), n);
  }
  if (error.empty() && (Wait(a.get(), 0ms) == WaitResult::kSuccess || Wait(b.get(), 0ms) == WaitResult::kSuccess)) {
    error = "a unit was left over: WaitAll took one without the other";
  }
  stop = true;
  a->Release(kConsumers, nullptr);
  b->Release(kConsumers, nullptr);
  for (auto& t : threads) {
    t.join();
  }
  if (error.empty() && timeouts) {
    error = Format("%d waits timed out", timeouts.load());
  }
  // A manual-reset event in a WaitAll stays signaled; a partial set takes nothing.
  if (error.empty()) {
    auto ev = Event::CreateManualResetEvent(true);
    auto sem = Semaphore::Create(1, 1);
    auto empty = Semaphore::Create(0, 1);
    if (WaitAll({sem.get(), empty.get()}, 0ms) == WaitResult::kSuccess) {
      error = "WaitAll succeeded with one object not signaled";
    } else if (WaitAll({ev.get(), sem.get()}, 0ms) != WaitResult::kSuccess) {
      error = "WaitAll failed with both signaled";
    } else if (Wait(ev.get(), 0ms) != WaitResult::kSuccess) {
      error = "WaitAll reset a manual-reset event";
    } else if (Wait(sem.get(), 0ms) == WaitResult::kSuccess) {
      error = "WaitAll did not take the semaphore";
    }
  }
  *detail = Format("%d pairs, %d threads", n, kConsumers);
  return error;
}

// A mutant: one owner at a time, recursion, Release only by the owner.
std::string MutantRules(double scale, std::string* detail) {
  constexpr int kThreads = 4;
  const int loops = Scaled(scale, 3000);
  auto m = Mutant::Create(false);
  std::atomic<int> inside{0}, overlap{0}, bad{0}, timeouts{0};
  int counter = 0;  // protected by the mutant
  std::vector<std::thread> threads;
  for (int k = 0; k < kThreads; ++k) {
    threads.emplace_back([&] {
      for (int i = 0; i < loops; ++i) {
        if (Wait(m.get(), kPatience) != WaitResult::kSuccess) {
          ++timeouts;
          return;
        }
        if (Wait(m.get(), 0ms) != WaitResult::kSuccess) {
          ++bad;  // recursion
        }
        if (inside.fetch_add(1) != 0) {
          ++overlap;
        }
        ++counter;
        inside.fetch_sub(1);
        if (!m->Release() || !m->Release()) {
          ++bad;
        }
      }
      if (m->Release()) {
        ++bad;  // not the owner any more
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  std::string error;
  if (overlap || bad || timeouts || counter != kThreads * loops) {
    error = Format("%d overlaps, %d bad recursions/releases, %d timeouts, count %d of %d", overlap.load(), bad.load(),
                   timeouts.load(), counter, kThreads * loops);
  }
  // Initially owned: another thread waits until the owner releases it.
  if (error.empty()) {
    auto owned = Mutant::Create(true);
    WaitResult before = WaitResult::kFailed, after = WaitResult::kFailed;
    bool released_by_other = true;
    std::thread other([&] {
      before = Wait(owned.get(), 20ms);
      released_by_other = owned->Release();
    });
    other.join();
    if (before != WaitResult::kTimeout || released_by_other) {
      error = "an owned mutant was taken or released by another thread";
    } else if (!owned->Release()) {
      error = "the owner could not release";
    } else {
      std::thread taker([&] {
        after = Wait(owned.get(), kPatience);
        owned->Release();
      });
      taker.join();
      if (after != WaitResult::kSuccess) {
        error = "a released mutant could not be taken";
      }
    }
  }
  *detail = Format("%d threads x %d acquisitions", kThreads, loops);
  return error;
}

// Timeouts end on time, alone and in WaitMultiple.
std::string Timeouts(double, std::string* detail) {
  auto ev = Event::CreateManualResetEvent(false);
  auto ev2 = Event::CreateAutoResetEvent(false);
  auto sem = Semaphore::Create(0, 1);
  double worst_late = 0;
  for (auto timeout : {1ms, 5ms, 30ms, 120ms}) {
    for (int which = 0; which < 3; ++which) {
      const auto t0 = Clock::now();
      WaitResult r;
      if (which == 0) {
        r = Wait(ev.get(), timeout);
      } else if (which == 1) {
        r = WaitAny({ev.get(), ev2.get(), sem.get()}, timeout).first;
      } else {
        r = WaitAll({ev.get(), sem.get()}, timeout);
      }
      const double took = Ms(Clock::now() - t0);
      if (r != WaitResult::kTimeout) {
        return Format("a %lld ms wait did not time out", (long long)timeout.count());
      }
#if BO2ZM_FAST_WAITS_HOOKS
      constexpr double kEarly = 0.05;  // ours checks the clock: never early
#else
      constexpr double kEarly = 1.0;  // Windows ends short waits up to a timer tick early
#endif
      if (took + kEarly < double(timeout.count())) {
        return Format("a %lld ms wait ended after %.3f ms", (long long)timeout.count(), took);
      }
      worst_late = std::max(worst_late, took - double(timeout.count()));
    }
  }
  if (worst_late > 250) {
    return Format("a timeout ended %.1f ms late", worst_late);
  }
  *detail = Format("worst %.3f ms late", worst_late);
  return "";
}

// Alertable waits end when a user callback (APC) is queued to the thread; non-alertable ones do not.
std::string Alertable(double, std::string* detail) {
  auto ev = Event::CreateManualResetEvent(false);
  auto ev2 = Event::CreateAutoResetEvent(false);
  std::atomic<int> stage{0};
  std::atomic<int64_t> queued_at{0};
  WaitResult results[4] = {};
  double latency[4] = {};
  rex::thread::SleepResult sleep_results[2] = {};
  auto since_queued = [&] {
    return Ms(Clock::now().time_since_epoch() - Clock::duration(queued_at.load()));
  };
  // A runtime thread, as guest threads are, and the object Create returns: on Windows the one from
  // GetCurrentThread() wraps the pseudo-handle, so callbacks queued through it would go to the caller.
  rex::thread::Thread::CreationParameters params;
  params.stack_size = 256 * 1024;
  auto t = rex::thread::Thread::Create(params, [&] {
    stage = 1;
    results[0] = Wait(ev.get(), 5000ms, true);
    latency[0] = since_queued();
    stage = 2;
    sleep_results[0] = rex::thread::AlertableSleep(5000ms);
    latency[1] = since_queued();
    stage = 3;
    results[1] = Wait(ev.get(), 100ms, false);  // a callback arrives meanwhile: must not end this wait
    sleep_results[1] = rex::thread::AlertableSleep(0ms);  // ...but is delivered here
    stage = 4;
    results[2] = WaitAny({ev.get(), ev2.get()}, 5000ms, true).first;
    latency[2] = since_queued();
    stage = 5;
  });
  if (!t) {
    return "could not create the thread";
  }
  auto queue = [&](int at_stage) {
    WaitUntil([&] { return stage.load() == at_stage; }, kPatience);
    std::this_thread::sleep_for(20ms);  // let it get into the wait
    queued_at = Clock::now().time_since_epoch().count();
    t->QueueUserCallback([] {});
  };
  queue(1);
  queue(2);
  queue(3);
  queue(4);
  // Thread objects are waitable too: this is the exit wait.
  if (Wait(t.get(), 4 * kPatience) != WaitResult::kSuccess) {
    return "the thread did not finish (waiting on its handle)";
  }
  if (results[0] != WaitResult::kUserCallback) {
    return "an alertable Wait did not end with the callback";
  }
  if (sleep_results[0] != rex::thread::SleepResult::kAlerted) {
    return "AlertableSleep did not end with the callback";
  }
  if (results[1] != WaitResult::kTimeout) {
    return "a non-alertable Wait ended with a callback";
  }
  if (sleep_results[1] != rex::thread::SleepResult::kAlerted) {
    return "the pending callback was not delivered by AlertableSleep(0)";
  }
  if (results[2] != WaitResult::kUserCallback) {
    return "an alertable WaitAny did not end with the callback";
  }
  *detail = Format("callback to wake-up: Wait %.3f ms, AlertableSleep %.3f ms, WaitAny %.3f ms", latency[0],
                   latency[1], latency[2]);
  if (std::max({latency[0], latency[1], latency[2]}) > 1000) {
    return "a callback took more than a second to end the wait";
  }
  return "";
}

// Many threads, random operations on shared objects, short timeouts: nothing hangs, mutant exclusion holds,
// and the objects still work afterwards.
std::string Storm(double scale, std::string* detail) {
  constexpr int kThreads = 8;
  const auto duration = std::chrono::milliseconds(std::max(200, int(3000 * scale)));
  std::unique_ptr<Event> events[4] = {Event::CreateAutoResetEvent(false), Event::CreateAutoResetEvent(false),
                                      Event::CreateManualResetEvent(false), Event::CreateManualResetEvent(false)};
  auto sem = Semaphore::Create(0, 100);
  auto mutant = Mutant::Create(false);
  std::atomic<rex::thread::Thread*> threads_seen[kThreads] = {};
  std::atomic<int> inside{0}, overlap{0}, mutant_timeouts{0}, quiet{0}, finished{0};
  std::atomic<uint64_t> ops{0}, successes{0}, callbacks{0};
  const auto end = Clock::now() + duration;
  std::vector<std::thread> threads;
  for (int k = 0; k < kThreads; ++k) {
    threads.emplace_back([&, k] {
      threads_seen[k] = rex::thread::Thread::GetCurrentThread();
      std::minstd_rand rng(1234 + k);
      WaitHandle* waitables[5] = {events[0].get(), events[1].get(), events[2].get(), events[3].get(), sem.get()};
      while (Clock::now() < end) {
        const auto timeout = std::chrono::milliseconds(rng() % 4);
        switch (rng() % 12) {
          case 0:
          case 1:
            events[rng() % 4]->Set();
            break;
          case 2:
            events[rng() % 4]->Reset();
            break;
          case 3:
            events[rng() % 4]->Pulse();
            break;
          case 4:
            sem->Release(1 + rng() % 3, nullptr);
            break;
          case 5:
          case 6:
            successes += Wait(waitables[rng() % 5], timeout) == WaitResult::kSuccess;
            break;
          case 7: {
            const size_t first = rng() % 5, second = (first + 1 + rng() % 4) % 5;
            successes += WaitAny({waitables[first], waitables[second], waitables[rng() % 5]}, timeout).first ==
                         WaitResult::kSuccess;
          } break;
          case 8: {
            const size_t first = rng() % 5, second = (first + 1 + rng() % 4) % 5;
            successes += WaitAll({waitables[first], waitables[second]}, timeout) == WaitResult::kSuccess;
          } break;
          case 9:
            if (Wait(mutant.get(), 200ms) == WaitResult::kSuccess) {
              if (inside.fetch_add(1) != 0) {
                ++overlap;
              }
              inside.fetch_sub(1);
              mutant->Release();
            } else {
              ++mutant_timeouts;
            }
            break;
          case 10:
            Wait(waitables[rng() % 5], timeout, true);
            break;
          case 11:
            if (auto* other = threads_seen[rng() % kThreads].load()) {
              other->QueueUserCallback([&] { ++callbacks; });
            }
            break;
        }
        ++ops;
      }
      // Once nobody queues callbacks any more, deliver what is still queued to this thread.
      ++quiet;
      WaitUntil([&] { return quiet.load() == kThreads; }, 10s);
      while (rex::thread::AlertableSleep(0ms) == rex::thread::SleepResult::kAlerted) {
      }
      ++finished;
    });
  }
  const bool all_finished = WaitUntil([&] { return finished.load() == kThreads; }, duration + 10s);
  if (!all_finished) {
    for (auto& t : threads) {
      t.detach();  // a stuck thread: the test fails, and the process keeps going
    }
    return Format("%d of %d threads still stuck 10 s after the end", kThreads - finished.load(), kThreads);
  }
  for (auto& t : threads) {
    t.join();
  }
  std::string error;
  if (overlap) {
    error = Format("mutant held by two threads %d times", overlap.load());
  } else if (mutant_timeouts) {
    error = Format("%d mutant waits timed out (200 ms) while it was only held for an instant", mutant_timeouts.load());
  }
  // Still working afterwards.
  for (int i = 0; i < 4 && error.empty(); ++i) {
    events[i]->Reset();
    if (Wait(events[i].get(), 0ms) == WaitResult::kSuccess) {
      error = Format("event %d signaled after Reset", i);
    }
    events[i]->Set();
    if (Wait(events[i].get(), 0ms) != WaitResult::kSuccess) {
      error = Format("event %d not signaled after Set", i);
    }
  }
  while (error.empty() && Wait(sem.get(), 0ms) == WaitResult::kSuccess) {
  }
  if (error.empty() && (!sem->Release(1, nullptr) || Wait(sem.get(), 0ms) != WaitResult::kSuccess ||
                        Wait(sem.get(), 0ms) == WaitResult::kSuccess)) {
    error = "the semaphore miscounts after the storm";
  }
  if (error.empty() && (Wait(mutant.get(), 0ms) != WaitResult::kSuccess || !mutant->Release())) {
    error = "the mutant is stuck after the storm";
  }
  *detail = Format("%llu operations in %lld ms, %llu waits satisfied, %llu callbacks run",
                   (unsigned long long)ops.load(), (long long)duration.count(),
                   (unsigned long long)successes.load(), (unsigned long long)callbacks.load());
  return error;
}

// The shape of BO2's job system: three workers wait on an event with 1 ms timeouts while one thread pulses it as
// fast as it can (NtPulseEvent from sub_8284A910). Reports the cost of a pulse.
std::string PulseRate(double scale, std::string* detail) {
  constexpr int kWorkers = 3;
  const auto duration = std::chrono::milliseconds(std::max(200, int(1000 * scale)));
  auto ev = Event::CreateManualResetEvent(false);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> wakes{0};
  std::vector<std::thread> workers;
  for (int k = 0; k < kWorkers; ++k) {
    workers.emplace_back([&] {
      while (!stop) {
        wakes += Wait(ev.get(), 1ms) == WaitResult::kSuccess;
      }
    });
  }
  std::this_thread::sleep_for(5ms);
  uint64_t pulses = 0;
  const auto start = Clock::now();
  while (Clock::now() - start < duration) {
    for (int i = 0; i < 64; ++i) {
      ev->Pulse();
    }
    pulses += 64;
  }
  const double ms = Ms(Clock::now() - start);
  stop = true;
  for (auto& t : workers) {
    t.join();
  }
  *detail = Format("%.2f us per pulse, %llu pulses, %.2f wake-ups per pulse", ms * 1000.0 / double(pulses),
                   (unsigned long long)pulses, double(wakes.load()) / double(pulses));
  return "";
}

// The guest clock (rex::chrono::Clock::QueryGuestTickCount, what the game's mftb reads): four threads read it as
// fast as they can. It must run at its frequency, never go back on a thread, and a read that follows another
// thread's read (through an atomic hand-off) must not be smaller.
std::string GuestClock(double scale, std::string* detail) {
  using Guest = rex::chrono::Clock;
  constexpr int kThreads = 4;
  const auto duration = std::chrono::milliseconds(std::max(200, int(1000 * scale)));
  std::atomic<bool> go{false}, stop{false};
  std::atomic<uint64_t> reads{0}, backwards{0};
  std::vector<std::thread> threads;
  for (int k = 0; k < kThreads; ++k) {
    threads.emplace_back([&] {
      while (!go) {
        std::this_thread::yield();
      }
      uint64_t last = Guest::QueryGuestTickCount(), n = 0;
      while (!stop) {
        const uint64_t now = Guest::QueryGuestTickCount();
        backwards += now < last;
        last = now;
        ++n;
      }
      reads += n;
    });
  }
  const uint64_t g0 = Guest::QueryGuestTickCount();
  const auto t0 = Clock::now();
  go = true;
  std::this_thread::sleep_for(duration);
  const uint64_t g1 = Guest::QueryGuestTickCount();
  const auto t1 = Clock::now();
  stop = true;
  for (auto& t : threads) {
    t.join();
  }
  const double seconds = std::chrono::duration<double>(t1 - t0).count();
  const double expected = double(Guest::guest_tick_frequency()) * Guest::guest_time_scalar();
  const double rate_error = (double(g1 - g0) / seconds) / expected - 1.0;
  const double ns_per_read = seconds * 1e9 * kThreads / double(std::max<uint64_t>(1, reads.load()));

  // Hand-offs: each side reads the clock, publishes the value, and the other reads after seeing it.
  const int handoffs = Scaled(scale, 20000);
  std::atomic<uint64_t> published{0};
  std::atomic<int> turn{0};
  std::atomic<int> out_of_order{0};
  std::thread other([&] {
    for (int i = 0; i < handoffs; ++i) {
      while (turn.load(std::memory_order_acquire) != 1) {
      }
      if (Guest::QueryGuestTickCount() < published.load(std::memory_order_relaxed)) {
        ++out_of_order;
      }
      published.store(Guest::QueryGuestTickCount(), std::memory_order_relaxed);
      turn.store(0, std::memory_order_release);
    }
  });
  for (int i = 0; i < handoffs; ++i) {
    while (turn.load(std::memory_order_acquire) != 0) {
    }
    if (Guest::QueryGuestTickCount() < published.load(std::memory_order_relaxed)) {
      ++out_of_order;
    }
    published.store(Guest::QueryGuestTickCount(), std::memory_order_relaxed);
    turn.store(1, std::memory_order_release);
  }
  other.join();

  *detail = Format("%.1f ns per read with %d threads, rate %+.3f %% of %.0f MHz, %llu reads, %d hand-offs",
                   ns_per_read, kThreads, rate_error * 100.0, expected / 1e6, (unsigned long long)reads.load(),
                   2 * handoffs);
  if (backwards) {
    return Format("went back %llu times on a thread", (unsigned long long)backwards.load());
  }
  if (out_of_order) {
    return Format("%d reads were smaller than another thread's earlier read", out_of_order.load());
  }
  if (std::abs(rate_error) > 0.005) {
    return Format("runs at %+.2f %% of its frequency", rate_error * 100.0);
  }
  return "";
}

#if !defined(BO2ZM_TEST_WAITS_STANDALONE)
// The handle table's lock-free lookups (sdk/src/system/util/object_table.cpp): two threads create and
// close kernel events while four look the handles up and use what they get. A lookup must return nothing or a
// live object; a use after free would crash or fail the type check. Needs the app's kernel (not in the Linux
// build).
std::string HandleTable(double scale, std::string* detail) {
  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state) {
    *detail = "no kernel yet: skipped";
    return "";
  }
  auto* table = kernel_state->object_table();
  constexpr int kSlots = 64, kWriters = 2, kReaders = 4;
  const auto duration = std::chrono::milliseconds(std::max(200, int(2000 * scale)));
  std::atomic<uint32_t> handles[kSlots] = {};
  std::atomic<uint64_t> created{0}, found{0}, missing{0}, bad{0};
  const auto end = Clock::now() + duration;
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      std::minstd_rand rng(77 + w);
      while (Clock::now() < end) {
        auto ev = rex::system::object_ref<rex::system::XEvent>(new rex::system::XEvent(kernel_state));
        ev->Initialize(false, false);
        const uint32_t old = handles[rng() % kSlots].exchange(ev->handle());
        if (old) {
          table->ReleaseHandle(old);  // closes it: the table drops its reference
        }
        ++created;
      }
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::minstd_rand rng(99 + r);
      while (Clock::now() < end) {
        const uint32_t handle = handles[rng() % kSlots].load();
        if (!handle) {
          continue;
        }
        if (auto ev = table->LookupObject<rex::system::XEvent>(handle)) {
          if (ev->type() != rex::system::XObject::Type::Event) {
            ++bad;
          }
          ev->Set(0, false);
          ev->Reset();
          ++found;
        } else {
          ++missing;
        }
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  uint32_t closed[kSlots];
  for (int i = 0; i < kSlots; ++i) {
    closed[i] = handles[i].exchange(0);
    if (closed[i]) {
      table->ReleaseHandle(closed[i]);
    }
  }
  int still_found = 0;
  for (int i = 0; i < kSlots; ++i) {
    if (closed[i] && table->LookupObject<rex::system::XEvent>(closed[i])) {
      ++still_found;
    }
  }
  *detail = Format("%llu events created and closed, %llu lookups found one, %llu found it closed",
                   (unsigned long long)created.load(), (unsigned long long)found.load(),
                   (unsigned long long)missing.load());
  if (bad) {
    return Format("%llu lookups returned something that was not an event", (unsigned long long)bad.load());
  }
  if (still_found) {
    return Format("%d closed handles still found", still_found);
  }
  return "";
}
#endif

}  // namespace

bool Run(double scale, const std::function<void(const std::string&)>& log) {
  struct Entry {
    const char* name;
    Test test;
  };
  const Entry tests[] = {
      {"rules", Rules},
      {"ping_pong", PingPong},
      {"auto_one_per_set", AutoOnePerSet},
      {"manual_set", ManualSet},
      {"manual_set_reset", ManualSetReset},
      {"manual_pulse", ManualPulse},
      {"auto_pulse", AutoPulse},
      {"semaphore", SemaphoreCounts},
      {"wait_any", WaitAnyCounts},
      {"wait_all", WaitAllCounts},
      {"mutant", MutantRules},
      {"timeouts", Timeouts},
      {"alertable", Alertable},
      {"storm", Storm},
      {"pulse_rate", PulseRate},
      {"guest_clock", GuestClock},
#if !defined(BO2ZM_TEST_WAITS_STANDALONE)
      {"handle_table", HandleTable},
#endif
  };
#if BO2ZM_FAST_WAITS_HOOKS
  const uint64_t lost_before = rex::thread::FastWaitLostWakeups();
#endif
  int failed = 0;
  const auto start = Clock::now();
  for (const Entry& entry : tests) {
    std::string detail;
    const auto t0 = Clock::now();
    const std::string error = entry.test(scale, &detail);
    const double ms = Ms(Clock::now() - t0);
    if (error.empty()) {
      log(Format("[test_waits] PASS %-17s %8.0f ms  %s", entry.name, ms, detail.c_str()));
    } else {
      ++failed;
      log(Format("[test_waits] FAIL %-17s %8.0f ms  %s (%s)", entry.name, ms, error.c_str(), detail.c_str()));
    }
  }
  int total = int(sizeof(tests) / sizeof(tests[0]));
#if BO2ZM_FAST_WAITS_HOOKS
  const uint64_t lost = rex::thread::FastWaitLostWakeups() - lost_before;
  ++total;
  if (lost) {
    ++failed;
    log(Format("[test_waits] FAIL lost_wakeups      the runtime reported %llu lost wake-ups", (unsigned long long)lost));
  } else {
    log("[test_waits] PASS lost_wakeups      none reported by the runtime");
  }
#endif
  log(Format("[test_waits] %s: %d of %d passed in %.1f s (scale %.2f)", failed ? "FAIL" : "PASS", total - failed,
             total, Ms(Clock::now() - start) / 1000.0, scale));
  return failed == 0;
}

#if !defined(BO2ZM_TEST_WAITS_STANDALONE)
void RunIfRequested() {
  // Once per process: a title relaunch (Zombies boot, mode switches) runs the app's set-up again.
  static std::atomic<bool> ran{false};
  if (!REXCVAR_GET(bo2zm_test_waits) || ran.exchange(true)) {
    return;
  }
  // About a quarter of the Linux run: a few seconds at start-up.
  Run(0.25, [](const std::string& line) { REXLOG_INFO("{}", line); });
}
#endif

}  // namespace bo2zm::test_waits
