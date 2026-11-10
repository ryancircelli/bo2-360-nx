/**
 * @file        input/sdl/pad_location_stub.cpp
 * @brief       Port and Bluetooth lookup for platforms whose SDL path already names the port.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/sdl/pad_location.h>

namespace rex::input {

PadLocation FindPadLocation(SDL_Gamepad*) {
  return {};
}

std::vector<HidIdentity> ListHidIdentities() {
  return {};
}

}  // namespace rex::input
