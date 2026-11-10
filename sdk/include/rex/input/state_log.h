#pragma once
/**
 * @file        rex/input/state_log.h
 * @brief       Human-readable text for pad state, for logs and the pad tester.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string>

#include <rex/input/input.h>

namespace rex::input {

/// Held buttons by name ("A LB Start"), or "-" when none.
std::string ButtonNames(uint16_t buttons);

/// "buttons=[A LB] LT=0 RT=255 L=(0,-32768) R=(120,4)".
std::string FormatGamepad(const X_INPUT_GAMEPAD& gamepad);

}  // namespace rex::input
