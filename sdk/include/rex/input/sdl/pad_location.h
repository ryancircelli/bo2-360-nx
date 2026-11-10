#pragma once
/**
 * @file        rex/input/sdl/pad_location.h
 * @brief       Finds which USB port or Bluetooth device an SDL gamepad is.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string>
#include <cstdint>
#include <vector>

struct SDL_Gamepad;

namespace rex::input {

struct PadLocation {
  std::string location;    // hex IOKit LocationID of a USB port, empty if none
  std::string bt_address;  // "aa:bb:cc:dd:ee:ff" for Bluetooth pads, empty if none
  std::string bt_id;       // IOKit PhysicalDeviceUniqueID, Bluetooth pads only
  std::string transport;   // IOKit Transport ("USB", "Bluetooth Low Energy", ...)
  std::string note;        // how it was found, or why not
};

/// macOS: reads the pad's HID device in IOKit, reached through the SDL path
/// ("DevSrvsID:<registry id>", HIDAPI pads) or, for pads SDL drives through the
/// GameController framework, through that controller's HID service. USB pads
/// give their port's LocationID, Bluetooth pads their device address (and
/// unique id). Other platforms: empty (the SDL path is used instead).
/// Call right after SDL_OpenGamepad, before the pad's player index is set.
PadLocation FindPadLocation(SDL_Gamepad* pad);

/// One IOKit HID device and what FindPadLocation would read from it.
struct HidIdentity {
  std::string product;
  uint32_t vendor_id = 0;
  uint32_t product_id = 0;
  uint64_t registry_id = 0;
  PadLocation location;
};

/// Every HID device IOKit lists (macOS; empty elsewhere), for rex-padtest
/// --hid: shows the port or Bluetooth address of pads SDL has not opened.
std::vector<HidIdentity> ListHidIdentities();

}  // namespace rex::input
