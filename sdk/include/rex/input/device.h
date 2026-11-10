#pragma once
/**
 * @file        rex/input/device.h
 * @brief       Input device identity shared by drivers and device assignment.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <string>

namespace rex::input {

constexpr uint32_t kMaxGuestUsers = 4;

/// Driver-scoped device handle. Never reused within a process run, so a handle
/// that outlives its device resolves to nothing rather than aliasing whichever
/// device took the freed slot.
enum class DeviceId : uint64_t { kInvalid = 0 };

struct DeviceInfo {
  DeviceId id = DeviceId::kInvalid;
  uint32_t ordinal = 0;  // connection order, assigned by InputSystem
  std::string name;
  std::string guid;
  bool synthetic = false;  // keyboard/mouse emulation or the NOP stand-in

  // Identity used to give a pad its player back after a reconnect, and to pin
  // it to a player from the config (input_player_slots). Empty when unknown.
  std::string serial;
  uint16_t vendor_id = 0;
  uint16_t product_id = 0;
  // A pad made by software rather than plugged in: Steam Input's virtual pads,
  // keyboard-to-pad mappers. Kept off player 1 unless input_virtual_pads says
  // otherwise.
  bool virtual_device = false;
  // A synthetic device that is a player of its own (keyboard and mouse with
  // mnk_player set). -1: synthetic devices share player 1, pads use ordinal.
  int32_t fixed_user = -1;

  // Diagnostics only: "usb", "bluetooth" or empty, and whether the pad says it
  // can rumble.
  std::string transport;
  bool can_rumble = false;
};

/// Stable text naming one physical pad: its serial when the backend reports
/// one, otherwise its GUID. Two identical pads without serials share a key.
std::string DeviceIdentityKey(const DeviceInfo& info);

}  // namespace rex::input
