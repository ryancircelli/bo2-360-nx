#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/cvar.h>

// Input/HID configuration flags
REXCVAR_DECLARE(std::string, guide_button);
REXCVAR_DECLARE(int32_t, guide_hold_ms);

namespace rex::input {
// guide_button: the game sees the Guide button (off, or the old "true").
inline bool GuideButtonToGame() {
  const std::string& mode = REXCVAR_GET(guide_button);  // a reference: no copy per pad poll
  return mode == "off" || mode == "true";
}
// guide_button: a long press opens the Guide.
inline bool GuideButtonHold() { return REXCVAR_GET(guide_button) == "hold"; }
}  // namespace rex::input
REXCVAR_DECLARE(std::string, hid_mappings_file);
REXCVAR_DECLARE(std::string, input_backend);
