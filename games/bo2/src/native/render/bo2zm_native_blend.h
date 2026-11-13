// Blend enable follows the channels written by the color attachment.
#pragma once

#include <cstdint>

namespace bo2zm::native {

constexpr bool ColorBlendEnabled(uint32_t mask, uint32_t control) {
  const bool rgb_direct = (control & 0x1F) == 1 && ((control >> 8) & 0x1F) == 0 &&
                          ((control >> 5) & 0x7) == 0;
  const bool alpha_direct = ((control >> 16) & 0x1F) == 1 && ((control >> 24) & 0x1F) == 0 &&
                            ((control >> 21) & 0x7) == 0;
  return ((mask & 0x7) && !rgb_direct) || ((mask & 0x8) && !alpha_direct);
}

}  // namespace bo2zm::native
