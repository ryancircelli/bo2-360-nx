// Plan a depth resolve spanning several row-aligned EDRAM surfaces.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bo2zm::native {

struct DepthResolveBank {
  uint32_t base;
  uint32_t width;
  uint32_t height;
};

struct DepthResolveRegion {
  size_t bank;
  uint32_t source_y;
  uint32_t destination_y;
  uint32_t height;
};

struct DepthResolvePlan {
  std::array<DepthResolveRegion, 8> regions{};
  size_t count = 0;
};

// Only unscaled, single-sample, 32-bit depth with matching row pitches is supported here.
// A later surface starts at its EDRAM row, even when the preceding host image has spare rows.
// An incomplete plan leaves the original resolve path in charge; never submit a partial plan.
inline DepthResolvePlan PlanDepthResolve(uint32_t base, uint32_t pitch, uint32_t y, uint32_t height,
                                        std::span<const DepthResolveBank> banks) {
  constexpr uint32_t kTileWidth = 80, kTileHeight = 16, kTiles = 2048;
  DepthResolvePlan plan;
  if (!pitch || pitch > 16383 || base >= kTiles || !height) {
    return plan;
  }
  const uint32_t tiles_per_row = (pitch + kTileWidth - 1) / kTileWidth;
  const uint64_t end = uint64_t(y) + height;
  if (end > UINT32_MAX || base + ((end + kTileHeight - 1) / kTileHeight) * tiles_per_row > kTiles) {
    return plan;  // Wrapped EDRAM ranges need a separate aliasing path.
  }
  const auto row_of = [&](const DepthResolveBank& bank) {
    if (bank.base < base || bank.base >= kTiles || bank.width != pitch || !bank.height ||
        (bank.base - base) % tiles_per_row) {
      return UINT32_MAX;
    }
    return (bank.base - base) / tiles_per_row * kTileHeight;
  };
  const uint32_t first_y = y;
  while (uint64_t(y) < end) {
    size_t selected = banks.size();
    uint32_t selected_row = 0;
    uint32_t next_row = uint32_t(end);
    for (size_t i = 0; i < banks.size(); ++i) {
      const uint32_t row = row_of(banks[i]);
      if (row == UINT32_MAX) {
        continue;
      }
      if (row > y) {
        next_row = std::min(next_row, row);
      } else if (uint64_t(y) < uint64_t(row) + banks[i].height &&
                 (selected == banks.size() || row > selected_row)) {
        selected = i;
        selected_row = row;
      }
    }
    if (selected == banks.size() || plan.count == plan.regions.size()) {
      return {};
    }
    const uint32_t last_y = uint32_t(std::min<uint64_t>(next_row, uint64_t(selected_row) + banks[selected].height));
    plan.regions[plan.count++] = {selected, y - selected_row, y - first_y, last_y - y};
    y = last_y;
  }
  return plan;
}

}  // namespace bo2zm::native
