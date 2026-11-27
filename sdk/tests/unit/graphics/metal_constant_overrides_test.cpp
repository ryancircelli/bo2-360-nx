#include "rex/graphics/metal/metal_constant_overrides.h"
#include <array>
#include <cstdlib>
#include <iostream>
using namespace rex::graphics::metal;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed line " << __LINE__ << '\n'; std::abort(); } } while (0)
int main() {
  std::vector<MetalConstantOverride> values;
  CHECK(ParseMetalConstantOverrides("# blur\nps E68FE55ECD482EC5 253 3 0.5\n", values));
  CHECK(values.size() == 1 && values[0].value == 0.5f);
  // metal_shader_constants form: ';' separates entries.
  CHECK(ParseMetalConstantOverrides("ps E68FE55ECD482EC5 253 3 0.5; vs 1 0 0 2", values));
  CHECK(values.size() == 2 && values[0].pixel && !values[1].pixel && values[1].value == 2.0f);
  CHECK(!ParseMetalConstantOverrides("ps 1 0 0 1; vs 1 0 0", values) && values.size() == 2);
  CHECK(ParseMetalConstantOverrides("ps E68FE55ECD482EC5 253 3 0.5", values));
  uint32_t exact; std::memcpy(&exact, &values[0].value, sizeof(exact));
  CHECK(exact == 0x3F000000);
  for (const char* bad : {"ps xyz 253 3 .5", "ps -1 0 0 1", "ps 1 256 0 1", "ps 1 1 4 1", "cs 1 0 0 1", "ps 1 0 0 nan", "ps 1 0 0 1 extra", "ps 1 -1 0 1", "ps 1 0 0"}) {
    CHECK(!ParseMetalConstantOverrides(bad, values));
    CHECK(values.size() == 1 && values[0].value == 0.5f);
  }
  // Sparse register packing: c0 precedes c253; unused constants take no space.
  uint64_t bitmap[4] = {1, 0, 0, uint64_t(1) << 61};
  std::array<float, 8> constants = {1,2,3,4,5,6,7,8};
  auto original = constants;
  auto apply = [&](bool ps, uint64_t hash, size_t bytes) {
    ApplyMetalConstantOverrides(values, ps, hash, bitmap,
        reinterpret_cast<uint8_t*>(constants.data()), bytes);
  };
  apply(false, values[0].hash, sizeof(constants)); CHECK(constants == original);
  apply(true, values[0].hash + 1, sizeof(constants)); CHECK(constants == original);
  apply(true, values[0].hash, sizeof(constants)-1); CHECK(constants == original);
  apply(true, values[0].hash, sizeof(constants));
  CHECK(constants[7] == 0.5f);
  for (unsigned i=0; i<7; ++i) CHECK(constants[i] == original[i]);
  CHECK(ParseMetalConstantOverrides("", values) && values.empty());
  constants = original; apply(true, 0, sizeof(constants)); CHECK(constants == original);
  std::cout << "constant override tests passed\n";
}
