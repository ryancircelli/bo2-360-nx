/**
 * @file        game_info.h
 * @brief       Facts about the running title that SDK code reads without
 *              naming a game. Titles fill them in RegisterGame()
 *              (rex/game_api.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>

namespace rex {

/// Facts about the running title that SDK code needs without naming it.
/// Defaults are the values the SDK used before titles registered themselves.
struct GameInfo {
  /// Short id: config file stem, Switch folder (sdmc:/switch/<short_name>).
  std::string short_name = "rex";
  /// First guest port the title's system link uses; the online relay carries
  /// three ports from here (the last one is the SDK's side channel).
  uint16_t first_link_port = 1000;
};

/// The process-wide title facts, set by RegisterGame() before setup.
inline GameInfo& Game() {
  static GameInfo info;
  return info;
}

}  // namespace rex
