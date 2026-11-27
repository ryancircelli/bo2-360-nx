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

#include <mutex>
#include <unordered_map>

#include <rex/chrono/chrono.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

namespace rex::system {

namespace {
// Save points: what the last SetTimer armed (guest file time, unscaled guest period). Kept beside the
// object so XTimer's layout (built into other libraries) does not change.
struct ArmInfo {
  uint32_t type = 0;
  bool armed = false;
  uint64_t due_file_time = 0;
  uint32_t period_guest_ms = 0;
};
std::mutex g_arm_m;
std::unordered_map<const XTimer*, ArmInfo> g_arm;
}  // namespace

XTimer::XTimer(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
  std::lock_guard<std::mutex> l(g_arm_m);
  g_arm.erase(this);
}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  {
    std::lock_guard<std::mutex> l(g_arm_m);
    g_arm[this].type = timer_type;
  }
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

  {
    std::lock_guard<std::mutex> l(g_arm_m);
    auto& a = g_arm[this];
    a.armed = true;
    a.period_guest_ms = period_ms;
    a.due_file_time = due_time < 0 ? chrono::Clock::QueryGuestSystemTime() + uint64_t(-due_time) : uint64_t(due_time);
  }
  period_ms = chrono::Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = rex::chrono::hundrednanoseconds(-due_time);
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::from_file_time(due_time));
  }

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    callback = [this]() {
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
  {
    std::lock_guard<std::mutex> l(g_arm_m);
    g_arm[this].armed = false;
  }
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

bool XTimer::Save(stream::ByteStream* stream) {
  SaveObject(stream);
  ArmInfo a;
  {
    std::lock_guard<std::mutex> l(g_arm_m);
    a = g_arm[this];
  }
  stream->Write<uint32_t>(a.type);
  stream->Write<bool>(a.armed);
  stream->Write<uint64_t>(a.due_file_time);
  stream->Write<uint32_t>(a.period_guest_ms);
  stream->Write<uint32_t>(callback_routine_);
  stream->Write<uint32_t>(callback_routine_arg_);
  stream->Write<uint32_t>(callback_thread_ ? callback_thread_->handle() : 0);
  return true;
}

// Re-arms from the restored guest clock; restore threads first (the callback thread is looked up).
object_ref<XTimer> XTimer::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto timer = object_ref<XTimer>(new XTimer(nullptr));
  timer->kernel_state_ = kernel_state;
  if (!timer->RestoreObject(stream)) return nullptr;
  timer->Initialize(stream->Read<uint32_t>());
  bool armed = stream->Read<bool>();
  uint64_t due = stream->Read<uint64_t>();
  uint32_t period = stream->Read<uint32_t>(), routine = stream->Read<uint32_t>(), arg = stream->Read<uint32_t>();
  uint32_t thread_handle = stream->Read<uint32_t>();
  if (!armed) return timer;
  uint64_t now = chrono::Clock::QueryGuestSystemTime();
  if (due <= now && period) {
    const uint64_t p = uint64_t(period) * 10000;  // guest ms -> 100 ns
    due += ((now - due) / p + 1) * p;
  }
  // shortcut: a one-shot timer that had already fired comes back unsignaled; fine while no title waits on one.
  if (due <= now) return timer;
  timer->SetTimer(-int64_t(due - now), period, routine, arg, false);
  auto thread = kernel_state->object_table()->LookupObject<XThread>(thread_handle);
  timer->callback_thread_ = thread.get();  // SetTimer took the (non-guest) loading thread
  return timer;
}

}  // namespace rex::system
