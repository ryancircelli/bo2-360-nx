// Guest clock rate: guest seconds follow host seconds times the time scalar (what `cmd clock_status` reports).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include <rex/chrono/clock.h>

using rex::chrono::Clock;

namespace {
// Guest seconds per host second over ~200 ms of wall time.
double MeasuredRate() {
  const uint64_t g0 = Clock::QueryGuestTickCount(), h0 = Clock::QueryHostTickCount();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const double guest_s = double(Clock::QueryGuestTickCount() - g0) / double(Clock::guest_tick_frequency());
  const double host_s = double(Clock::QueryHostTickCount() - h0) / double(Clock::QueryHostTickFrequency());
  return guest_s / host_s;
}
}  // namespace

TEST_CASE("guest clock runs at host speed times the time scalar", "[clock]") {
  REQUIRE_FALSE(Clock::frame_locked());
  Clock::set_guest_tick_frequency(50000000);  // Xenon timebase
  Clock::set_guest_time_scalar(1.0);
  const double r1 = MeasuredRate();
  CHECK(r1 > 0.98);
  CHECK(r1 < 1.02);
  Clock::set_guest_time_scalar(2.0);
  const double r2 = MeasuredRate();
  CHECK(r2 > 1.96);
  CHECK(r2 < 2.04);
  Clock::set_guest_time_scalar(1.0);
}
