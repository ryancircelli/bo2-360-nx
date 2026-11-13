// Presentation-only targets start a new composition with their first resolved-color pass.
#pragma once

#include <cstdint>
#include "bo2zm_native_blend.h"

namespace bo2zm::native {

struct FrontWriteRect {
  float left = 0, top = 0, right = 0, bottom = 0;
};

// A full write also requires COLOR0 output and no shader or alpha-test discard.
constexpr bool FrontOpaqueRGBAWrite(uint32_t mask, uint32_t blend, uint32_t color_control,
                                    bool color0, bool discards) {
  const bool alpha_test = (color_control & 8) && (color_control & 7) != 7;
  return (mask & 0xF) == 0xF && !ColorBlendEnabled(mask, blend) && color0 && !discards && !alpha_test;
}

// Same conservative validity rule as the target cache's redundant-clear check.
// Swaps do not invalidate it; any intervening draw or content replacement does.
constexpr bool FrontKnownClearValid(bool clean, uint64_t drawn_on_clear, uint64_t drawn) {
  return clean && drawn_on_clear == drawn;
}

enum class FrontDecision {
  kAlreadyWritten, kRetained, kKnownClear, kNoResolvedInput, kNonOpaqueWrite, kInvalidBounds, kInitialize
};

constexpr const char* FrontDecisionName(FrontDecision decision) {
  switch (decision) {
    case FrontDecision::kAlreadyWritten: return "already-written";
    case FrontDecision::kRetained: return "retained";
    case FrontDecision::kKnownClear: return "known-clear";
    case FrontDecision::kNoResolvedInput: return "no-resolved-input";
    case FrontDecision::kNonOpaqueWrite: return "non-opaque-write";
    case FrontDecision::kInvalidBounds: return "invalid-bounds";
    case FrontDecision::kInitialize: return "initialize";
  }
  return "unknown";
}

struct FrontComposition {
  uint32_t address = 0;
  uint32_t width = 0, height = 0;
  uint64_t write_frame = UINT64_MAX;

  FrontDecision Inspect(uint64_t frame, bool swap_only, FrontWriteRect rect, bool resolved_input = true,
                        bool opaque_rgba = true, bool known_clear = false) const {
    if (write_frame == frame) return FrontDecision::kAlreadyWritten;
    if (!swap_only) return FrontDecision::kRetained;
    if (known_clear) return FrontDecision::kKnownClear;
    if (!resolved_input) return FrontDecision::kNoResolvedInput;
    if (!opaque_rgba) return FrontDecision::kNonOpaqueWrite;
    // Empty, unknown and out-of-range bounds keep the retained-image path.
    if (!width || !height || !(rect.left >= 0 && rect.top >= 0 &&
        rect.right <= float(width) && rect.bottom <= float(height) &&
        rect.right > rect.left && rect.bottom > rect.top)) return FrontDecision::kInvalidBounds;
    // The viewport limits rasterization, not the submitted geometry's coverage. A scene
    // composite can use an inset quad inside a full viewport, so either starts the composition.
    return FrontDecision::kInitialize;
  }

  bool Begin(uint64_t frame, bool swap_only, FrontWriteRect rect, bool resolved_input = true,
              bool opaque_rgba = true, bool known_clear = false) {
    const auto decision = Inspect(frame, swap_only, rect, resolved_input, opaque_rgba, known_clear);
    write_frame = frame;
    return decision == FrontDecision::kInitialize;
  }
};

}  // namespace bo2zm::native
