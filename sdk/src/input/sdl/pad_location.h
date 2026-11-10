#pragma once
/**
 * @file        input/sdl/pad_location.h
 * @brief       Finds which USB port an SDL gamepad is plugged into.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string>

#include <SDL3/SDL_gamepad.h>

namespace rex::input {

struct PadLocation {
  std::string location;  // hex IOKit LocationID, empty when not found
  std::string note;      // how it was found, or why not
};

/// macOS: the IOKit LocationID of the pad's HID device, reached through the
/// SDL path ("DevSrvsID:<registry id>", HIDAPI pads) or, for pads SDL drives
/// through the GameController framework, through that controller's HID
/// service. Other platforms: empty (the SDL path is used instead).
/// Call right after SDL_OpenGamepad, before the pad's player index is set.
PadLocation FindPadLocation(SDL_Gamepad* pad);

}  // namespace rex::input
