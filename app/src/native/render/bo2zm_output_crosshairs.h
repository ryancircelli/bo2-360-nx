// std140 payload for independent crosshairs in the complete output image.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace bo2zm::native::output {
struct Crosshairs {
  float rects[4][4]{};
  uint32_t styles[4][4]{};  // enabled, type, color, reserved
  bool Any() const {
    for (const auto& style : styles) if (style[0]) return true;
    return false;
  }
};
static_assert(sizeof(Crosshairs) == 128 && offsetof(Crosshairs, styles) == 64);

// Everything the output pass (bo2zm_output_gamma_ramp.frag) draws with, for a presenter that records it itself
// (the Metal one; the Vulkan path records it in bo2zm_native_targets.cpp PaintOutput).
struct Pass {
  static constexpr size_t kFloatsTable = 256 * 4 + 8;  // the Ramp block before its crosshairs
  bool ramp = false;  // false: the image as is (no table, no overlays)
  bool exact = false;    // specialization constant 0
  bool grading = false;  // 1
  bool fxaa = false;     // 2
  float table[kFloatsTable]{};
  Crosshairs crosshairs;
  // The push constants from byte 16: displacement, inverse size, counter, note, banner, banner_info.
  uint32_t constants[24]{};
};

template<class Overlay> Crosshairs PackCrosshairs(const Overlay* records, int count) {
  Crosshairs result;
  if (!records) return result;
  for (int i = 0; i < std::clamp(count, 0, 4); ++i) {
    const auto& record = records[i];
    if (!record.enabled || record.type < 0 || record.type > 1 || record.color < 0 || record.color > 6) continue;
    const float* r = record.normalizedRect;
    bool finite = true;
    for (int j = 0; j < 4; ++j) finite &= std::isfinite(r[j]);
    if (!finite || r[0] < 0 || r[1] < 0 || r[2] <= 0 || r[3] <= 0 ||
        r[0] + r[2] > 1.000001f || r[1] + r[3] > 1.000001f) continue;
    result.rects[i][0] = r[0]; result.rects[i][1] = r[1];
    result.rects[i][2] = std::min(r[2], 1.0f - r[0]);
    result.rects[i][3] = std::min(r[3], 1.0f - r[1]);
    result.styles[i][0] = 1;
    result.styles[i][1] = uint32_t(record.type);
    result.styles[i][2] = uint32_t(record.color);
  }
  return result;
}
}  // namespace bo2zm::native::output
