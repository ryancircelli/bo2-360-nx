/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/thread/mutex.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(guest_msr_lock, true, "CPU",
                    "Guest mfmsr / mtmsrd r13 interrupt-disable sections take the SDK global lock "
                    "(false = skip it; the sections wrap lwarx/stwcx. loops that are atomic by "
                    "compare-and-swap)");
REXCVAR_DEFINE_INT32(guest_msr_lock_flip_s, 0, "CPU",
                     "In-run A/B: flip guest_msr_lock every N seconds and log each flip (0 = off)");

namespace rex::thread {

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

namespace {

// Number of guest interrupt-disable sections currently holding the lock (any thread).
std::atomic<int32_t> guest_lock_count{0};
std::atomic<bool> guest_lock_on{true};
// Per thread: depth of sections that took the lock / that skipped it. A section
// nested in a locked one also locks, one nested in a skipped one also skips, so a
// runtime flip never unbalances the mutex.
thread_local int32_t locked_depth = 0;
thread_local int32_t skipped_depth = 0;

void StartOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    guest_lock_on.store(REXCVAR_GET(guest_msr_lock));
    const int32_t flip_s = REXCVAR_GET(guest_msr_lock_flip_s);
    if (flip_s <= 0) return;
    auto log_state = [](bool on) {
      auto epoch_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
      REXLOG_INFO("guest_msr_lock_flip on={} epoch_us={}", on ? 1 : 0, epoch_us);
    };
    log_state(guest_lock_on.load());
    std::thread([flip_s, log_state] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(flip_s));
        const bool on = !guest_lock_on.load();
        guest_lock_on.store(on);
        log_state(on);
      }
    }).detach();
  });
}

}  // namespace

uint64_t global_critical_region::GuestCheck() {
  StartOnce();
  if (guest_lock_on.load(std::memory_order_relaxed)) {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    return guest_lock_count.load() ? 0 : 0x8000;
  }
  return guest_lock_count.load(std::memory_order_relaxed) ? 0 : 0x8000;
}

void global_critical_region::GuestEnter() {
  StartOnce();
  if (skipped_depth == 0 && (locked_depth > 0 || guest_lock_on.load(std::memory_order_relaxed))) {
    mutex().lock();
    guest_lock_count.fetch_add(1);
    ++locked_depth;
  } else {
    ++skipped_depth;
  }
}

void global_critical_region::GuestLeave() {
  if (skipped_depth > 0) {
    --skipped_depth;
    return;
  }
  if (locked_depth > 0) --locked_depth;
  [[maybe_unused]] auto old_count = guest_lock_count.fetch_sub(1);
  assert(old_count >= 1 && "LeaveGlobalLock called without matching EnterGlobalLock");
  mutex().unlock();
}

}  // namespace rex::thread
