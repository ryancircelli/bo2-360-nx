/**
 * @file        runtime_stack_range_test.cpp
 * @brief       Guest stacks stay below the physical alias range
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xthread.h>

TEST_CASE("The guest stack range ends below the physical alias range", "[runtime][stack]") {
  using rex::system::XThread;
  CHECK(XThread::kStackAddressRangeBegin < XThread::kStackAddressRangeEnd);
  CHECK(XThread::kStackAddressRangeEnd < 0xE0000000u);
}
