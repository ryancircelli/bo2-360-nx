// Standalone texture-detail checks; no GPU, SDK or game data required.
// clang++ -std=c++20 tools/tests/native_texture_mips.cpp -o native_texture_mips
#include "../../games/bo2/src/native/render/bo2zm_native_texture_mips.h"

#include <array>
#include <cassert>
#include <iostream>

using namespace bo2zm::native;

int main() {
  // NativeShaders removes whole vertex-fetch instructions before computing these identities.
  constexpr uint64_t ui = 0x58C4C9291CEDFCA7ull;
  constexpr uint64_t icon = 0xB60067497710DCF4ull;
  constexpr uint64_t unknown = 0x123456789ABCDEF0ull;
  constexpr uint32_t depth_off = 0xF0;

  // Pack coverage reports unmasked microcode hashes, which are not runtime UI identities.
  for (uint64_t raw : {0x0479CB710D805FF3ull, 0x4A8255A17452248Dull}) {
    assert(!IsUiTextureDraw(raw, depth_off));
    assert(TextureMipFloor(raw, depth_off, 2, 0) == 2);
  }

  // Boot, map preset application, return to menus, preset change, diagnostic on/off.
  constexpr std::array presets{0u, 2u, 2u, 3u, 1u, 0u, 2u, 2u, 2u};
  constexpr std::array diagnostics{0u, 0u, 0u, 0u, 0u, 0u, 1u, 4u, 0u};
  for (size_t i = 0; i < presets.size(); ++i) {
    for (uint64_t shader : {ui, icon}) {
      assert(TextureMipFloor(shader, depth_off, presets[i], diagnostics[i]) == diagnostics[i]);
      assert(SamplerMipRange(0xA0, TextureMipFloor(shader, depth_off, presets[i], diagnostics[i])) ==
             (0xA0 | diagnostics[i]));
    }
    assert(TextureMipFloor(unknown, depth_off, presets[i], diagnostics[i]) ==
           std::max(presets[i], diagnostics[i]));
  }

  // Sharing the UI shader does not exempt world geometry using depth/stencil.
  for (uint32_t preset = 0; preset <= 4; ++preset) {
    for (uint32_t diagnostic = 0; diagnostic <= 4; ++diagnostic) {
      for (uint32_t control = 0; control <= 0xFF; ++control) {
        for (uint64_t shader : {ui, icon, unknown, uint64_t(0)}) {
          const uint32_t floor = TextureMipFloor(shader, control, preset, diagnostic);
          if (shader == unknown || !shader || (control & 7)) {
            assert(floor == std::max(preset, diagnostic));
          } else {
            assert(floor == diagnostic);
          }
        }
      }
    }
  }

  // Exhaust the guest mip limits, including base-only, missing base, and reversed limits.
  // Compare the world result with the original sampler's minimum-level rule.
  for (uint32_t mips = 0; mips <= 0xFF; ++mips) {
    const uint32_t guest_min = mips & 15;
    const uint32_t guest_max = mips >> 4;
    assert(SamplerMipRange(mips, 0) == mips);
    for (uint32_t floor = 0; floor <= 4; ++floor) {
      const uint32_t result = SamplerMipRange(mips, floor);
      assert((result >> 4) == guest_max);
      const uint32_t expected_min = guest_max > guest_min ? std::min(guest_max, guest_min + floor) : guest_min;
      assert((result & 15) == expected_min);
    }
  }
  assert(TextureMipFloor(unknown, depth_off, 10, 0) == 4);
  assert(TextureMipFloor(ui, depth_off, 10, 0) == 0);
  assert(TextureMipFloor(ui, depth_off, 0, 10) == 4);
  std::cout << "Texture mip checks passed: UI guards, preset transitions, diagnostics and guest mip bounds\n";
}
