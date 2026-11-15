/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

REXCVAR_DEFINE_BOOL(timer_clamp_absolute_due, true, "Kernel",
                    "Clamp absolute timer due times to within 10 years of now, so a time long past fires at once "
                    "instead of overflowing the steady_clock conversion (false: old behaviour, for A/B)");
REXCVAR_DECLARE(int32_t, wait_watchdog_s);

namespace rex::system {

XTimer::XTimer(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() = default;

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = rex::thread::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = rex::thread::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                          uint32_t routine_arg, bool resume) {
  using rex::chrono::WinSystemClock;
  using rex::chrono::XSystemClock;
  // Caller is checking for STATUS_TIMER_RESUME_IGNORED.
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  period_ms = chrono::Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = rex::chrono::hundrednanoseconds(-due_time);
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    // An absolute guest FILETIME. Same arithmetic as clock_cast<WinSystemClock> (host now + guest time left, scaled),
    // but the time left is clamped to +-10 years first.
    //
    // Why: NFSMW re-arms its tick timer with an absolute due time of 0 (the year 1601) when the thread that stops
    // the timer writes 0 into the due-time slot between the tick thread's check of it and its re-arm. On the
    // console a past absolute time fires at once; here the time left, about -1.3e17 x 100 ns, became about
    // -1.3e19 ns in the later conversion to steady_clock (PosixTimer), which overflows int64 nanoseconds and wraps
    // to about 160 years ahead. The timer never fired, the tick thread waited forever, and the game stopped while
    // loading a race (PAS-18). Within +-10 years the result is unchanged; a past time still fires at once.
    constexpr auto kLimit = rex::chrono::hundrednanoseconds(10ll * 365 * 24 * 3600 * 10000000ll);
    auto left = rex::chrono::hundrednanoseconds(XSystemClock::from_file_time(due_time) - XSystemClock::now());
    const bool clamp = REXCVAR_GET(timer_clamp_absolute_due);
    if (left < -kLimit || left > kLimit) {
      REXSYS_WARN("XTimer: absolute due time {:016X} is {} s from guest now; {}", due_time,
                  left.count() / 10000000, clamp ? "clamped to +-10 years" : "NOT clamped (timer_clamp_absolute_due=false)");
    }
    if (!clamp) {
      due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::from_file_time(due_time));
    } else {
      left = std::clamp(left, -kLimit, kLimit);
      if (!REXCVAR_GET(clock_no_scaling)) {
        left = std::chrono::floor<WinSystemClock::duration>(left * chrono::Clock::guest_time_scalar());
      }
      due_tp = WinSystemClock::now() + left;
    }
  }

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    callback = [this]() {
      DiagNoteSignal();
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = rex::chrono::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      REXSYS_DEBUG("XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
                   callback_routine_, callback_routine_arg_, time_low, time_high);
      callback_thread_->EnqueueApc(callback_routine_, callback_routine_arg_, time_low, time_high);
    };
  }

  if (!callback && REXCVAR_GET(wait_watchdog_s) > 0) {
    callback = [this]() { DiagNoteSignal(); };  // diagnostics only
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result =
        timer_->SetRepeatingAt(due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::system
