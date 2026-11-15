// Early absolute NT due times must convert to a steady_clock point in the past
// (not overflow), so POSIX timers fire immediately.
#include <catch2/catch_test_macros.hpp>

#include <rex/chrono/chrono.h>
#include <rex/chrono/chrono_steady_cast.h>

using rex::chrono::WinSystemClock;

static std::chrono::steady_clock::time_point ToSteady(int64_t filetime_ticks) {
  WinSystemClock::time_point t{WinSystemClock::duration(filetime_ticks)};
  return std::chrono::clock_cast<std::chrono::steady_clock>(t);
}

TEST_CASE("early FILETIME due times convert to the past", "[chrono]") {
  const auto now = std::chrono::steady_clock::now();
  for (int64_t ft : {int64_t{0}, int64_t{1}, int64_t{166666}}) {
    REQUIRE(ToSteady(ft) < now);
  }
}

TEST_CASE("far-future FILETIME due times convert to the future", "[chrono]") {
  const auto now = std::chrono::steady_clock::now();
  REQUIRE(ToSteady(INT64_MAX / 2) > now);
}
