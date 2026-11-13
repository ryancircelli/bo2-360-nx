// CPU checks for the presentation-only composition boundary; no SDK or game data.
#include "../../games/bo2/src/native/render/bo2zm_native_frontbuffer.h"

#include <array>
#include <cassert>
#include <iostream>
#include <limits>

using namespace bo2zm::native;

int main() {
  // Actual traced composite: RGBA enabled, ONE/ZERO/ADD for RGB and alpha,
  // and alpha test disabled. Reserved blend bits do not affect the pipeline.
  constexpr uint32_t direct = 0x00010001;
  assert(!ColorBlendEnabled(0xF, direct));
  assert(!ColorBlendEnabled(0xF, direct | 0xE000E000));
  assert(FrontOpaqueRGBAWrite(0xF, direct, 0xAA000007, true, false));
  for (uint32_t missing = 1; missing <= 8; missing <<= 1) {
    assert(!FrontOpaqueRGBAWrite(0xF ^ missing, direct, 7, true, false));
  }
  // Destination-dependent RGB/alpha and non-ADD operations are not opaque.
  for (uint32_t change : {2u, 1u << 8, 1u << 5, 2u << 16, 1u << 24, 1u << 21}) {
    assert(ColorBlendEnabled(0xF, direct ^ change));
    assert(!FrontOpaqueRGBAWrite(0xF, direct ^ change, 7, true, false));
  }
  assert(ColorBlendEnabled(0xF, 0x010B0706)); // UI alpha blend from the trace.
  assert(!ColorBlendEnabled(7, 1)); // Alpha equation is ignored for RGB-only writes.
  assert(!ColorBlendEnabled(8, 1u << 16)); // RGB equation is ignored for alpha-only writes.
  assert(!ColorBlendEnabled(0, 0));
  assert(!FrontOpaqueRGBAWrite(0xF, direct, 7, false, false)); // No COLOR0 output.
  assert(!FrontOpaqueRGBAWrite(0xF, direct, 7, true, true)); // Shader can discard.
  for (uint32_t function = 0; function < 7; ++function) {
    assert(!FrontOpaqueRGBAWrite(0xF, direct, 8 | function, true, false));
  }
  assert(FrontOpaqueRGBAWrite(0xF, direct, 15, true, false)); // Alpha test ALWAYS cannot discard.

  constexpr FrontWriteRect whole{0, 0, 1280, 720};
  constexpr FrontWriteRect top{128, 0, 1152, 360};
  constexpr FrontWriteRect bottom{128, 360, 1152, 720};
  FrontComposition front{1, 1280, 720, 0};
  assert(front.Inspect(1, true, top) == FrontDecision::kInitialize);
  assert(front.Inspect(1, true, whole) == FrontDecision::kInitialize);
  assert(front.Inspect(1, true, top, false) == FrontDecision::kNoResolvedInput);
  assert(front.Inspect(1, false, top) == FrontDecision::kRetained);
  assert(front.Inspect(1, true, {}) == FrontDecision::kInvalidBounds);
  assert(front.write_frame == 0); // Diagnostic inspection must not consume the first write.
  assert(front.Begin(1, true, top));
  assert(front.Inspect(1, true, bottom) == FrontDecision::kAlreadyWritten);
  assert(!front.Begin(1, true, bottom)); // Must not erase the first view.
  assert(!front.Begin(1, true, whole)); // HUD and later passes stay intact.
  assert(front.Begin(2, true, bottom)); // Either player may be drawn first.
  assert(!front.Begin(2, true, top));
  assert(front.Begin(3, true, whole)); // A full viewport may contain an inset scene quad.
  assert(!front.Begin(3, true, top)); // No late clear after the first scene composite.
  assert(!front.Begin(4, false, top)); // Sampled buffers, expired ownership, or rollback switch.
  assert(!front.Begin(4, true, bottom)); // Do not clear after earlier unqualified writes either.
  front.write_frame = 5; // A guest color clear already started the frame.
  assert(!front.Begin(5, true, top));

  // A blended/partial first write consumes the slot; a later opaque player/HUD
  // cannot turn that retained composition into a late automatic clear.
  FrontComposition blended{4, 1280, 720, 0};
  assert(blended.Inspect(1, true, whole, true, false) == FrontDecision::kNonOpaqueWrite);
  assert(!blended.Begin(1, true, whole, true, false));
  assert(!blended.Begin(1, true, whole, true, true));

  // Existing target-cache validity uses the global recorded-draw counter. A Swap
  // alone does not change it. Replaced contents or any draw make this proof stale.
  assert(FrontKnownClearValid(true, 42, 42));
  assert(!FrontKnownClearValid(false, 42, 42));
  assert(!FrontKnownClearValid(true, 42, 43));
  FrontComposition explicit_clear{5, 1280, 720, 0};
  for (uint64_t swap = 1; swap < 4; ++swap) {
    const bool valid = FrontKnownClearValid(true, 42, 42);
    assert(explicit_clear.Inspect(swap, true, whole, true, true, valid) == FrontDecision::kKnownClear);
    assert(!explicit_clear.Begin(swap, true, whole, true, true, valid));
    assert(!explicit_clear.Begin(swap, true, top)); // Never retry after the first decision.
  }
  assert(explicit_clear.Begin(4, true, whole, true, true, FrontKnownClearValid(true, 42, 43)));

  // An attempted clear that fails before binding still consumes the first-write decision.
  // The renderer reports that failure and passes swap_only=false thereafter (session fallback).
  assert(front.Begin(6, true, top));
  assert(!front.Begin(6, true, bottom));
  assert(!front.Begin(7, false, top));
  assert(!front.Begin(7, false, bottom));

  // Asset-only UI keeps its background with either a full or inset viewport. A first asset
  // write also consumes the slot, preventing a later resolved UI panel from erasing it.
  assert(!front.Begin(8, true, whole, false));
  assert(!front.Begin(8, true, top));
  assert(!front.Begin(9, true, top, false));
  assert(!front.Begin(9, true, bottom, true));

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (FrontWriteRect invalid : std::array{FrontWriteRect{}, FrontWriteRect{100, 0, 50, 20},
       FrontWriteRect{-1, 0, 100, 20}, FrontWriteRect{0, 0, 1281, 720},
       FrontWriteRect{nan, 0, 100, 20}, FrontWriteRect{0, 0, inf, 720}}) {
    front.write_frame = UINT64_MAX;
    assert(!front.Begin(6, true, invalid));
  }
  FrontComposition unknown{};
  assert(!unknown.Begin(1, true, top));

  // Model two independently moving views over an old loading picture, with untouched allocation padding.
  constexpr unsigned width = 10, height = 8, allocation_height = 10;
  std::array<unsigned, width * allocation_height> pixels;
  pixels.fill(9);
  FrontComposition composition{2, width, height, 0};
  const auto pass = [&](uint64_t frame, FrontWriteRect viewport, FrontWriteRect geometry, unsigned color,
                        bool resolved_input = true, bool opaque_rgba = true, bool known_clear = false) {
    if (composition.Begin(frame, true, viewport, resolved_input, opaque_rgba, known_clear)) {
      for (unsigned i = 0; i < width * height; ++i) pixels[i] = 0;
    }
    for (unsigned y = unsigned(geometry.top); y < unsigned(geometry.bottom); ++y) {
      for (unsigned x = unsigned(geometry.left); x < unsigned(geometry.right); ++x) pixels[y * width + x] = color;
    }
  };
  constexpr FrontWriteRect viewport{0, 0, width, height};
  for (uint64_t frame = 1; frame < 20; ++frame) {
    // Submitted quads cover separate views even when both draws use the full viewport.
    pass(frame, viewport, {1, 0, 9, 4}, unsigned(frame + 1));
    pass(frame, viewport, {1, 4, 9, 8}, unsigned(frame + 2));
    for (unsigned y = 0; y < allocation_height; ++y) {
      for (unsigned x = 0; x < width; ++x) {
        const unsigned expected = y >= height ? 9 : x == 0 || x == width - 1 ? 0 :
                                  y < 4 ? unsigned(frame + 1) : unsigned(frame + 2);
        assert(pixels[y * width + x] == expected);
      }
    }
  }
  // Independent retained render targets have independent state and never qualify as Swap-only.
  FrontComposition retained{3, width, height, 0};
  assert(!retained.Begin(20, false, {1, 0, 9, 4}));
  assert(!composition.Begin(19, true, {1, 0, 9, 4}));
  // A single full-screen scene overwrites the initialized visible extent; asset HUD draws
  // retain that scene, and an asset-only next frame keeps all pixels outside its panel.
  pass(20, viewport, viewport, 42);
  pass(20, viewport, {2, 2, 4, 4}, 43, false);
  pass(21, viewport, {2, 2, 4, 4}, 44, false);
  pass(21, viewport, {4, 2, 6, 4}, 45, true); // No late clear for a resolved UI panel.
  for (unsigned y = 0; y < allocation_height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      const unsigned expected = y >= height ? 9 : y >= 2 && y < 4 && x >= 2 && x < 4 ? 44 :
                                y >= 2 && y < 4 && x >= 4 && x < 6 ? 45 : 42;
      assert(pixels[y * width + x] == expected);
    }
  }
  // A known nonblack guest clear stays around an inset opaque quad after Swap;
  // a blended first panel similarly keeps all surrounding retained pixels.
  pixels.fill(7);
  pass(22, viewport, {1, 0, 9, 4}, 46, true, true, true);
  pass(22, viewport, {1, 4, 9, 8}, 47);
  for (unsigned y = 0; y < allocation_height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      const unsigned expected = y >= height || x == 0 || x == width - 1 ? 7 : y < 4 ? 46 : 47;
      assert(pixels[y * width + x] == expected);
    }
  }
  pass(23, viewport, {2, 2, 4, 4}, 48, true, false);
  pass(23, viewport, {4, 2, 6, 4}, 49);
  assert(pixels[0] == 7 && pixels[width - 1] == 7);
  assert(pixels[2 * width + 2] == 48 && pixels[2 * width + 4] == 49);
  std::cout << "Front composition checks passed: one initialization, two views, bounds, retention and padding\n";
}
