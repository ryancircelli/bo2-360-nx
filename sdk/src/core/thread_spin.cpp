// SPDX-License-Identifier: 0BSD
// Spin-wait detection: see include/rex/thread_spin.h.
#include <rex/thread_spin.h>

#include <algorithm>
#include <atomic>
#include <chrono>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

REXCVAR_DEFINE_BOOL(spin_wait_backoff, true, "Kernel",
                    "Guest threads that poll back to back (NtYieldExecution, zero-timeout waits and delays) sleep a "
                    "short, growing time after spin_wait_threshold polls, so a spinning thread leaves its core to the "
                    "ones that have work");
REXCVAR_DEFINE_INT32(spin_wait_threshold, 32, "Kernel", "Polls in a row before a spinning guest thread starts to sleep");
REXCVAR_DEFINE_INT32(spin_wait_gap_us, 200, "Kernel",
                     "Two polls closer together than this are one spin; a longer gap of other work ends it");
REXCVAR_DEFINE_INT32(spin_wait_step_us, 10, "Kernel", "Each poll in a spin sleeps this much longer than the last");
REXCVAR_DEFINE_INT32(spin_wait_max_us, 200, "Kernel", "The longest a spinning guest thread sleeps in one poll");

namespace rex::thread {

uint32_t SpinTracker::OnPoll(uint64_t now_ns, const Config& c) {
  if (last_end_ns == 0 || now_ns < last_end_ns || now_ns - last_end_ns > c.gap_ns) {
    run = 1;
    return 0;
  }
  if (run < 0xFFFFFFFFu) ++run;
  if (run <= c.threshold) return 0;
  const uint64_t sleep = uint64_t(run - c.threshold) * c.step_us;
  return uint32_t(std::min<uint64_t>(sleep, c.max_sleep_us));
}

namespace {
std::atomic<uint64_t> g_polls{0}, g_backoffs{0}, g_slept_us{0};
std::atomic<uint64_t> g_last_log_ns{0};
thread_local SpinTracker t_tracker;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}  // namespace

void NoteGuestPoll() {
  if (!REXCVAR_GET(spin_wait_backoff)) return;
  SpinTracker::Config c;
  c.gap_ns = uint32_t(std::max(1, REXCVAR_GET(spin_wait_gap_us))) * 1000u;
  c.threshold = uint32_t(std::max(1, REXCVAR_GET(spin_wait_threshold)));
  c.step_us = uint32_t(std::max(1, REXCVAR_GET(spin_wait_step_us)));
  c.max_sleep_us = uint32_t(std::max(1, REXCVAR_GET(spin_wait_max_us)));
  const uint64_t now = NowNs();
  g_polls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t us = t_tracker.OnPoll(now, c);
  if (us) {
    g_backoffs.fetch_add(1, std::memory_order_relaxed);
    g_slept_us.fetch_add(us, std::memory_order_relaxed);
    Sleep(std::chrono::microseconds(us));
    uint64_t last = g_last_log_ns.load(std::memory_order_relaxed);
    const uint64_t after = NowNs();
    if (after - last > 30'000'000'000ull && g_last_log_ns.compare_exchange_strong(last, after)) {
      const SpinStats s = GetSpinStats();
      REXKRNL_INFO("[spin] {} polls, {} backoffs, {} ms slept", s.polls, s.backoffs, s.slept_us / 1000);
    }
    t_tracker.OnReturn(after);
  } else {
    t_tracker.OnReturn(NowNs());
  }
}

SpinStats GetSpinStats() {
  return {g_polls.load(std::memory_order_relaxed), g_backoffs.load(std::memory_order_relaxed),
          g_slept_us.load(std::memory_order_relaxed)};
}

}  // namespace rex::thread
