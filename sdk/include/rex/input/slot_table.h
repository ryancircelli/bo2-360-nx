#pragma once
/**
 * @file        rex/input/slot_table.h
 * @brief       Chooses the player (ordinal) a newly connected pad takes.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/input/device.h>

namespace rex::input {

/// What to do with pads made by software (Steam Input, keyboard mappers).
enum class VirtualPadPolicy {
  kAfterPhysical,  // never player 1; a real pad gets player 1
  kNormal,         // treated like any other pad
  kIgnore,         // not offered to the game at all
};

/// "after", "normal" or "ignore"; nullopt for anything else.
std::optional<VirtualPadPolicy> ParseVirtualPadPolicy(std::string_view text);

/// One player pinned to a pad in the config.
struct SlotPin {
  uint32_t user = 0;  // 0-based guest user
  std::string match;  // serial, GUID, vid:pid (hex) or part of the name
};

/// Parses "1=<match>;2=<match>" (players are 1-based, entries split by ';' or
/// ','). Malformed entries are skipped and described in *error.
std::vector<SlotPin> ParseSlotPins(std::string_view text, std::string* error = nullptr);

/// Case-insensitive: equal to the serial, the GUID or "vvvv:pppp", or
/// contained in the name.
bool PinMatches(std::string_view match, const DeviceInfo& info);

/// Gives each newly connected pad a player and remembers the player of each pad
/// that leaves, so that pad gets the same player back when it reconnects, even
/// if other pads came and went meanwhile. Players already held never move.
///
/// Order of preference for a new pad:
///  0. its fixed_user, if it has one and that player is free (script pads);
///  1. the player the config pins it to, if free;
///  2. the player it had before it disconnected, if free;
///  3. the lowest free player nobody is pinned to or waiting to reconnect to;
///  4. the lowest free player nobody is pinned to (the waiting pad loses it);
///  5. the lowest free player;
///  6. past the last player (connected, but the game does not see it).
/// Virtual pads under kAfterPhysical skip player 1 in every step but 1.
class SlotTable {
 public:
  void SetPins(std::vector<SlotPin> pins) { pins_ = std::move(pins); }
  const std::vector<SlotPin>& pins() const { return pins_; }
  void SetVirtualPolicy(VirtualPadPolicy policy) { virtual_policy_ = policy; }
  VirtualPadPolicy virtual_policy() const { return virtual_policy_; }
  void SetRememberDisconnected(bool remember) { remember_ = remember; }

  /// `live` holds every device keeping its ordinal through this refresh,
  /// including synthetic devices with a fixed user, which hold that player.
  uint32_t AssignOrdinal(const DeviceInfo& fresh, const std::vector<DeviceInfo>& live);

  /// Remembers the pad's player for its reconnect.
  void OnDisconnected(const DeviceInfo& gone);

  /// Players waiting for a pad to come back, for logging.
  std::vector<std::pair<std::string, uint32_t>> Reservations() const;

 private:
  struct Reservation {
    std::string key;
    uint32_t ordinal;
  };

  std::vector<SlotPin> pins_;
  VirtualPadPolicy virtual_policy_ = VirtualPadPolicy::kAfterPhysical;
  bool remember_ = true;
  // Oldest first.
  std::vector<Reservation> reserved_;
};

}  // namespace rex::input
