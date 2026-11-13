// Authored field names exercise the boundary without game data or a runtime.
#include "../../games/bo2/src/bo2zm_gsc_canonical.h"
#include <cassert>
#include <iostream>

using bo2zm::gsc::CanonicalNames;
template <class F> void Reject(F fn) {
  bool rejected = false;
  try { fn(); } catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
}
int main() {
  CanonicalNames names;
  const auto stock = [](uint32_t id) { return "stock_" + std::to_string(id); };
  uint32_t count = CanonicalNames::kStockLimit;
  assert(names.Resolve(count, "stock_1", stock) == 1 && names.Count() == count);
  for (uint32_t id = count; id < CanonicalNames::kLimit; ++id) {
    const auto field = "mod_" + std::to_string(id);
    assert(names.Resolve(count, field, stock) == id);
    count = names.Count();
    assert(count == id + 1);
    assert(names.Resolve(count, field, stock) == id && names.Count() == count);
    assert(names.Resolve(count, "stock_1", stock) == 1);
  }
  Reject([&] { names.Resolve(count, "one_too_many", stock); });
  assert(names.Count() == CanonicalNames::kLimit);
  Reject([&] { names.Resolve(count - 1, "out_of_sync", stock); });
  Reject([&] { names.Resolve(1, "below_stock_boundary", stock); });
  Reject([&] { names.Resolve(count + 1, "outside_cap", stock); });
  names.Reset();
  assert(names.Count() == 0);
  count = CanonicalNames::kStockLimit;
  Reject([&] { names.Resolve(count, "new_generation", [](uint32_t) -> std::string {
    throw std::runtime_error("incomplete stock seed");
  }); });
  assert(names.Count() == 0);
  assert(names.Resolve(count, "new_generation", stock) == count);
  assert(names.Resolve(names.Count(), "mod_13500", stock) == count + 1);
  names.Reset();
  assert(names.Resolve(count, "duplicate", [](uint32_t) { return "duplicate"; }) == 1);
  assert(names.Count() == count);
  std::cout << "Canonical field registry fixtures passed\n";
}
