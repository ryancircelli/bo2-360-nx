// Test helper: virtual gamepad driven by a button script (bo2zm_test_buttons).
// See bo2zm_test_pad.cpp.

#pragma once

#include <rex/runtime.h>

namespace bo2zm::test_pad {

// Adds diagnostic script pads for bo2zm_test_buttons and bo2zm_test_buttons_p2,
// bo2zm_test_buttons_p3 and bo2zm_test_buttons_p4 on guest ports zero through three.
// Call in OnPreSetup, after ReXApp installed the default factory.
void WrapInput(rex::RuntimeConfig& config);

}  // namespace bo2zm::test_pad
