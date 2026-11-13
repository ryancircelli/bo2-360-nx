// CPU checks for depth resolves whose EDRAM rows span multiple host images.
#include "../../games/bo2/src/native/render/bo2zm_native_depth_resolve.h"

#include <cassert>
#include <iostream>

using namespace bo2zm::native;

int main() {
  const std::array banks = {DepthResolveBank{384, 1040, 1040}, DepthResolveBank{1216, 1040, 1040}};
  const auto full = PlanDepthResolve(384, 1040, 0, 2048, banks);
  assert(full.count == 2);
  assert(full.regions[0].bank == 0 && full.regions[0].source_y == 0 &&
         full.regions[0].destination_y == 0 && full.regions[0].height == 1024);
  assert(full.regions[1].bank == 1 && full.regions[1].source_y == 0 &&
         full.regions[1].destination_y == 1024 && full.regions[1].height == 1024);
  const auto crossing = PlanDepthResolve(384, 1040, 1000, 80, banks);
  assert(crossing.count == 2 && crossing.regions[0].source_y == 1000 &&
         crossing.regions[0].height == 24 && crossing.regions[1].source_y == 0 &&
         crossing.regions[1].destination_y == 24 && crossing.regions[1].height == 56);
  const std::array reverse = {banks[1], banks[0]};
  const auto reordered = PlanDepthResolve(384, 1040, 0, 2048, reverse);
  assert(reordered.count == 2 && reordered.regions[0].bank == 1 && reordered.regions[1].bank == 0);
  assert(PlanDepthResolve(384, 1040, 0, 2048, std::span(banks.data(), 1)).count == 0);
  auto malformed = banks;
  malformed[1].base++;
  assert(PlanDepthResolve(384, 1040, 0, 2048, malformed).count == 0);
  malformed = banks;
  malformed[1].width = 560;
  assert(PlanDepthResolve(384, 1040, 0, 2048, malformed).count == 0);
  malformed = banks;
  malformed[1].height = 512;
  assert(PlanDepthResolve(384, 1040, 0, 2048, malformed).count == 0);
  assert(PlanDepthResolve(384, 1040, 0, 2049, banks).count == 0);
  assert(PlanDepthResolve(384, 0, 0, 2048, banks).count == 0);
  assert(PlanDepthResolve(384, 1040, UINT32_MAX, 2, banks).count == 0);
  const auto single = PlanDepthResolve(384, 1040, 32, 720, banks);
  assert(single.count == 1 && single.regions[0].source_y == 32 && single.regions[0].height == 720);
  const std::array sparse = {DepthResolveBank{0, 80, 16}, DepthResolveBank{2, 80, 16}};
  assert(PlanDepthResolve(0, 80, 0, 48, sparse).count == 0);
  std::array<DepthResolveBank, 9> many{};
  for (uint32_t i = 0; i < many.size(); ++i) many[i] = {i, 80, 16};
  assert(PlanDepthResolve(0, 80, 0, 144, many).count == 0);

  // Verify every requested row independently, including intervals starting/ending within a bank.
  for (uint32_t first = 0; first < 2048; first += 13) {
    for (uint32_t length = 1; length <= 2048 - first; length += 127) {
      const auto plan = PlanDepthResolve(384, 1040, first, length, banks);
      assert(plan.count > 0);
      uint32_t covered = 0;
      for (size_t i = 0; i < plan.count; ++i) {
        const auto& region = plan.regions[i];
        assert(region.destination_y == covered && region.height > 0);
        for (uint32_t row = 0; row < region.height; ++row) {
          const uint32_t guest_y = first + covered + row;
          assert(region.bank == (guest_y >= 1024 ? 1u : 0u));
          assert(region.source_y + row == guest_y % 1024);
          assert(region.source_y + row < banks[region.bank].height);
        }
        covered += region.height;
      }
      assert(covered == length);
    }
  }
  std::cout << "Depth resolve checks passed: bank boundaries, offsets, bounds, gaps and row coverage\n";
}
