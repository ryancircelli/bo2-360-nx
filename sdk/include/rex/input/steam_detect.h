#pragma once
/**
 * @file        rex/input/steam_detect.h
 * @brief       Notices Steam and Steam Input, which can take controllers away
 *              from the game.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rex::input {

struct SteamInputReport {
  bool steam_running = false;      // a Steam client process is running
  bool launched_by_steam = false;  // Steam's launch variables are set
  // Steam Input settings read from Steam's config.vdf, as "Xbox=1". Best
  // effort: empty when the file is missing or the keys were not found.
  std::vector<std::string> settings;
  // Any of those settings turns Steam Input on for some controller family.
  bool steam_input_enabled = false;
};

SteamInputReport DetectSteamInput();

/// The warning to log, or empty when Steam is not around.
std::string SteamInputWarning(const SteamInputReport& report);

/// Steam Input's virtual pad ("Steam Virtual Gamepad", Valve 28de:11ff).
/// Physical Valve hardware (Steam Controller, Steam Deck) is not virtual.
bool IsSteamVirtualPad(uint16_t vendor_id, uint16_t product_id, std::string_view name);

}  // namespace rex::input
