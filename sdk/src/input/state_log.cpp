/**
 * @file        input/state_log.cpp
 * @brief       Human-readable text for pad state, for logs and the pad tester.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/state_log.h>

#include <array>
#include <utility>

#include <fmt/format.h>

namespace rex::input {

std::string ButtonNames(uint16_t buttons) {
  static constexpr std::array<std::pair<uint16_t, const char*>, 15> kNames = {{
      {X_INPUT_GAMEPAD_A, "A"},
      {X_INPUT_GAMEPAD_B, "B"},
      {X_INPUT_GAMEPAD_X, "X"},
      {X_INPUT_GAMEPAD_Y, "Y"},
      {X_INPUT_GAMEPAD_LEFT_SHOULDER, "LB"},
      {X_INPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
      {X_INPUT_GAMEPAD_LEFT_THUMB, "LS"},
      {X_INPUT_GAMEPAD_RIGHT_THUMB, "RS"},
      {X_INPUT_GAMEPAD_BACK, "Back"},
      {X_INPUT_GAMEPAD_START, "Start"},
      {X_INPUT_GAMEPAD_GUIDE, "Guide"},
      {X_INPUT_GAMEPAD_DPAD_UP, "Up"},
      {X_INPUT_GAMEPAD_DPAD_DOWN, "Down"},
      {X_INPUT_GAMEPAD_DPAD_LEFT, "Left"},
      {X_INPUT_GAMEPAD_DPAD_RIGHT, "Right"},
  }};
  std::string out;
  for (const auto& [bit, name] : kNames) {
    if (buttons & bit) {
      if (!out.empty()) {
        out += ' ';
      }
      out += name;
    }
  }
  return out.empty() ? "-" : out;
}

std::string FormatGamepad(const X_INPUT_GAMEPAD& gamepad) {
  return fmt::format("buttons=[{}] LT={} RT={} L=({},{}) R=({},{})",
                     ButtonNames(static_cast<uint16_t>(gamepad.buttons)), gamepad.left_trigger,
                     gamepad.right_trigger, static_cast<int16_t>(gamepad.thumb_lx),
                     static_cast<int16_t>(gamepad.thumb_ly), static_cast<int16_t>(gamepad.thumb_rx),
                     static_cast<int16_t>(gamepad.thumb_ry));
}

}  // namespace rex::input
