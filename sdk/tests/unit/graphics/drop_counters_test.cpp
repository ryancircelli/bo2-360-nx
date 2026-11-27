#include <rex/graphics/swap_hook.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

using rex::graphics::CountDrop;
using rex::graphics::DropCountersText;

TEST_CASE("drop counters count by reason text, across threads", "[graphics]") {
  CHECK(DropCountersText() == "none");
  // A second copy of a literal (as in another library) lands on the same row.
  static char other_copy[] = "b_reason";
  CHECK(CountDrop("b_reason") == 1);
  CHECK(CountDrop(other_copy, 4) == 5);
  CHECK(CountDrop("a_reason", 0) == 0);  // seen but zero: not listed
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([] {
      for (int i = 0; i < 1000; ++i) CountDrop("c_reason");
    });
  }
  for (auto& t : threads) t.join();
  CHECK(DropCountersText() == "b_reason=5 c_reason=4000");
}
