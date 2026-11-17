// Compat: v0.10 rex::chrono::Clock on v0.11 rex::Clock.
#pragma once
#include <rex/chrono.h>
namespace rex::chrono {
struct Clock {
  static uint64_t guest_tick_frequency() { return ::rex::Clock::GetGuestTickFrequency(); }
  static uint64_t QueryGuestTickCount() { return ::rex::Clock::QueryGuestTickCount(); }
  static uint64_t QueryGuestSystemTime() { return ::rex::Clock::QueryGuestSystemTime(); }
  static uint32_t QueryGuestUptimeMillis() { return ::rex::Clock::GetGuestUptimeMillis(); }
  static uint64_t QueryHostSystemTime() { return ::rex::Clock::QueryHostSystemTime(); }
  static uint64_t QueryHostTickCount() { return ::rex::Clock::GetHostTickCount(); }
  static uint64_t QueryHostTickFrequency() { return ::rex::Clock::GetHostTickFrequency(); }
  static uint64_t QueryHostUptimeMillis() { return ::rex::Clock::QueryHostUptimeMillis(); }
  static double guest_time_scalar() { return ::rex::Clock::GetGuestTimeScalar(); }
  static void set_guest_time_scalar(double s) { ::rex::Clock::SetGuestTimeScalar(s); }
};
}  // namespace rex::chrono
