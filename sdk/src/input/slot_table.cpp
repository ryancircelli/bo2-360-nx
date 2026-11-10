/**
 * @file        input/slot_table.cpp
 * @brief       Chooses the player (ordinal) a newly connected pad takes.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/slot_table.h>

#include <algorithm>
#include <cctype>
#include <charconv>

#include <fmt/format.h>

namespace rex::input {

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

std::string DeviceIdentityKey(const DeviceInfo& info) {
  if (!info.serial.empty()) {
    return "serial:" + Lower(info.serial);
  }
  if (!info.guid.empty()) {
    return "guid:" + Lower(info.guid);
  }
  return "name:" + Lower(info.name);
}

std::optional<VirtualPadPolicy> ParseVirtualPadPolicy(std::string_view text) {
  const std::string lower = Lower(Trim(text));
  if (lower == "after") {
    return VirtualPadPolicy::kAfterPhysical;
  }
  if (lower == "normal") {
    return VirtualPadPolicy::kNormal;
  }
  if (lower == "ignore") {
    return VirtualPadPolicy::kIgnore;
  }
  return std::nullopt;
}

std::vector<SlotPin> ParseSlotPins(std::string_view text, std::string* error) {
  std::vector<SlotPin> pins;
  std::string problems;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find_first_of(";,", start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view entry = Trim(text.substr(start, end - start));
    start = end + 1;
    if (entry.empty()) {
      continue;
    }
    const size_t eq = entry.find('=');
    const std::string_view player_text = eq == std::string_view::npos ? "" : Trim(entry.substr(0, eq));
    const std::string_view match = eq == std::string_view::npos ? "" : Trim(entry.substr(eq + 1));
    uint32_t player = 0;
    const auto parsed =
        std::from_chars(player_text.data(), player_text.data() + player_text.size(), player);
    if (eq == std::string_view::npos || match.empty() || parsed.ec != std::errc() ||
        parsed.ptr != player_text.data() + player_text.size() || player < 1 ||
        player > kMaxGuestUsers) {
      problems += fmt::format("{}'{}' is not <player 1-{}>=<serial, GUID, vid:pid or name>",
                              problems.empty() ? "" : "; ", entry, kMaxGuestUsers);
      continue;
    }
    pins.push_back({player - 1, std::string(match)});
  }
  if (error) {
    *error = problems;
  }
  return pins;
}

bool PinMatches(std::string_view match, const DeviceInfo& info) {
  const std::string needle = Lower(Trim(match));
  if (needle.empty()) {
    return false;
  }
  if (!info.serial.empty() && needle == Lower(info.serial)) {
    return true;
  }
  if (!info.guid.empty() && needle == Lower(info.guid)) {
    return true;
  }
  if ((info.vendor_id || info.product_id) &&
      needle == fmt::format("{:04x}:{:04x}", info.vendor_id, info.product_id)) {
    return true;
  }
  return !info.name.empty() && Lower(info.name).find(needle) != std::string::npos;
}

uint32_t SlotTable::AssignOrdinal(const DeviceInfo& fresh, const std::vector<DeviceInfo>& live) {
  auto taken = [&](uint32_t ordinal) {
    for (const auto& device : live) {
      if (device.synthetic) {
        if (device.fixed_user >= 0 && static_cast<uint32_t>(device.fixed_user) == ordinal) {
          return true;
        }
      } else if (device.ordinal == ordinal) {
        return true;
      }
    }
    return false;
  };
  auto pinned_to_other = [&](uint32_t ordinal) {
    return std::any_of(pins_.begin(), pins_.end(), [&](const SlotPin& pin) {
      return pin.user == ordinal && !PinMatches(pin.match, fresh);
    });
  };
  auto reserved = [&](uint32_t ordinal) {
    return std::any_of(reserved_.begin(), reserved_.end(),
                       [&](const Reservation& r) { return r.ordinal == ordinal; });
  };
  const std::string key = DeviceIdentityKey(fresh);
  auto take = [&](uint32_t ordinal) {
    // Anyone waiting for this player has lost it. A serial names one pad, so
    // its other reservation is spent too; a GUID may stand for an identical
    // pad still away, whose reservation stays.
    const bool unique = key.starts_with("serial:");
    std::erase_if(reserved_, [&](const Reservation& r) {
      return r.ordinal == ordinal || (unique && r.key == key);
    });
    return ordinal;
  };

  // 0. A pad that names its own player (script pads).
  if (!fresh.synthetic && fresh.fixed_user >= 0 &&
      static_cast<uint32_t>(fresh.fixed_user) < kMaxGuestUsers && !taken(fresh.fixed_user)) {
    return take(static_cast<uint32_t>(fresh.fixed_user));
  }

  // 1. Pinned in the config.
  for (const auto& pin : pins_) {
    if (pin.user < kMaxGuestUsers && PinMatches(pin.match, fresh) && !taken(pin.user)) {
      return take(pin.user);
    }
  }

  const uint32_t first =
      fresh.virtual_device && virtual_policy_ == VirtualPadPolicy::kAfterPhysical ? 1 : 0;

  // 2. Back to the player it had.
  for (const auto& r : reserved_) {
    if (r.key == key && r.ordinal >= first && r.ordinal < kMaxGuestUsers && !taken(r.ordinal)) {
      return take(r.ordinal);
    }
  }

  // 3-5. Lowest free player, relaxing one rule per pass.
  for (int pass = 0; pass < 3; pass++) {
    for (uint32_t ordinal = first; ordinal < kMaxGuestUsers; ordinal++) {
      if (taken(ordinal)) {
        continue;
      }
      if (pass < 2 && pinned_to_other(ordinal)) {
        continue;
      }
      if (pass < 1 && reserved(ordinal)) {
        continue;
      }
      return take(ordinal);
    }
  }

  // 6. Every player is held: park it where the game does not look.
  uint32_t ordinal = kMaxGuestUsers;
  while (taken(ordinal)) {
    ordinal++;
  }
  return ordinal;
}

void SlotTable::OnDisconnected(const DeviceInfo& gone) {
  if (!remember_ || gone.synthetic || gone.ordinal >= kMaxGuestUsers) {
    return;
  }
  const std::string key = DeviceIdentityKey(gone);
  // Two identical pads without serials share a key; each keeps its own player.
  if (std::any_of(reserved_.begin(), reserved_.end(), [&](const Reservation& r) {
        return r.key == key && r.ordinal == gone.ordinal;
      })) {
    return;
  }
  reserved_.push_back({key, gone.ordinal});
}

std::vector<std::pair<std::string, uint32_t>> SlotTable::Reservations() const {
  std::vector<std::pair<std::string, uint32_t>> out;
  for (const auto& r : reserved_) {
    out.emplace_back(r.key, r.ordinal);
  }
  return out;
}

}  // namespace rex::input
