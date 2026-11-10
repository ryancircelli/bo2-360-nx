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
#include <string_view>

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

  // Where the pad is plugged in, for pads with no serial: on macOS the IOKit
  // LocationID of its USB port, as hex ("14200000"). Stays the same when the
  // pad is replugged into the same port and across relaunches. Empty when
  // unknown; location_note then says why.
  std::string location;
  std::string location_note;
  // The backend's device path (SDL_GetGamepadPath). Used as identity only when
  // it names a port rather than one attachment (see IsStablePadPath).
  std::string path;
};

/// Which fact identifies a pad, strongest first.
enum class IdentitySource {
  kSerial,    // the pad's own serial number
  kLocation,  // the USB port it is plugged into (macOS IOKit LocationID)
  kPath,      // the backend's device path, when it names a port
  kGuid,      // model only: identical pads share it; connection order decides
  kName,      // nothing better known
};

struct DeviceIdentity {
  std::string key;       // e.g. "serial:...", "port:045e:0b12@14200000"
  IdentitySource source = IdentitySource::kName;
  std::string reason;    // one line, for logs and rex-padtest --list
  // Only one pad at a time can have this key (serial, port, path).
  bool unique = false;
};

/// Picks the strongest identity the pad has: serial, then port location, then
/// a port-naming path, then GUID, then name.
DeviceIdentity ResolveDeviceIdentity(const DeviceInfo& info);

/// The model-level key (GUID, else name) that identical pads share. Used when a
/// pad comes back in a different port, so it still finds its player by
/// connection order like before.
std::string DeviceModelKey(const DeviceInfo& info);

/// False for paths that name one attachment and change on every replug:
/// macOS "DevSrvsID:<registry id>", Linux /dev/input/eventN and /dev/hidrawN,
/// and XInput's "XInput#N" user slots.
bool IsStablePadPath(std::string_view path);

/// ResolveDeviceIdentity(info).key.
std::string DeviceIdentityKey(const DeviceInfo& info);

const char* IdentitySourceName(IdentitySource source);

}  // namespace rex::input
