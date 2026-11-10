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

bool IsStablePadPath(std::string_view path) {
  if (path.empty()) {
    return false;
  }
  for (std::string_view prefix : {"DevSrvsID:", "/dev/input/", "/dev/hidraw", "XInput#"}) {
    if (path.starts_with(prefix)) {
      return false;
    }
  }
  return true;
}

std::string DeviceModelKey(const DeviceInfo& info) {
  if (!info.guid.empty()) {
    return "guid:" + Lower(info.guid);
  }
  return "name:" + Lower(info.name);
}

DeviceIdentity ResolveDeviceIdentity(const DeviceInfo& info) {
  DeviceIdentity id;
  if (!info.serial.empty()) {
    id.key = "serial:" + Lower(info.serial);
    id.source = IdentitySource::kSerial;
    id.unique = true;
    id.reason = "the pad reports a serial number";
    return id;
  }
  if (!info.location.empty()) {
    id.key = fmt::format("port:{:04x}:{:04x}@{}", info.vendor_id, info.product_id,
                         Lower(info.location));
    id.source = IdentitySource::kLocation;
    id.unique = true;
    id.reason = fmt::format("no serial; {} port {}{}{}",
                            info.transport.empty() ? "connection" : info.transport,
                            Lower(info.location), info.location_note.empty() ? "" : ", ",
                            info.location_note);
    return id;
  }
  if (IsStablePadPath(info.path)) {
    id.key = "path:" + Lower(info.path);
    id.source = IdentitySource::kPath;
    id.unique = true;
    id.reason = "no serial or port location; the device path names the port";
    return id;
  }
  std::string why = "no serial";
  if (!info.location_note.empty()) {
    why += "; no port location (" + info.location_note + ")";
  }
  if (!info.path.empty()) {
    why += "; path \"" + info.path + "\" changes on every replug";
  }
  id.key = DeviceModelKey(info);
  id.source = info.guid.empty() ? IdentitySource::kName : IdentitySource::kGuid;
  id.reason = why + "; identical pads share this key, so connection order decides";
  return id;
}

std::string DeviceIdentityKey(const DeviceInfo& info) {
  return ResolveDeviceIdentity(info).key;
}

const char* IdentitySourceName(IdentitySource source) {
  switch (source) {
    case IdentitySource::kSerial:
      return "serial";
    case IdentitySource::kLocation:
      return "port";
    case IdentitySource::kPath:
      return "path";
    case IdentitySource::kGuid:
      return "guid+order";
    case IdentitySource::kName:
      return "name+order";
  }
  return "?";
}

std::vector<size_t> PlacementOrder(const std::vector<DeviceInfo>& pads) {
  std::vector<size_t> order(pads.size());
  std::vector<size_t> located;
  for (size_t i = 0; i < pads.size(); i++) {
    order[i] = i;
    if (!pads[i].synthetic && ResolveDeviceIdentity(pads[i]).source == IdentitySource::kLocation) {
      located.push_back(i);
    }
  }
  // Only pads placed by port trade places, and only among the positions they
  // already hold; every other pad keeps its connection-order position.
  std::vector<size_t> sorted = located;
  auto location_value = [&](size_t i) {
    uint64_t value = 0;
    const std::string& text = pads[i].location;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return parsed.ec == std::errc() ? value : UINT64_MAX;
  };
  std::stable_sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
    const auto& pa = pads[a];
    const auto& pb = pads[b];
    if (pa.vendor_id != pb.vendor_id) {
      return pa.vendor_id < pb.vendor_id;
    }
    if (pa.product_id != pb.product_id) {
      return pa.product_id < pb.product_id;
    }
    const uint64_t la = location_value(a);
    const uint64_t lb = location_value(b);
    if (la != lb) {
      return la < lb;
    }
    return pa.location < pb.location;
  });
  for (size_t k = 0; k < located.size(); k++) {
    order[located[k]] = sorted[k];
  }
  return order;
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
  if (!info.location.empty() &&
      (needle == Lower(info.location) || needle == "port:" + Lower(info.location) ||
       needle == ResolveDeviceIdentity(info).key)) {
    return true;
  }
  if (IsStablePadPath(info.path) &&
      (needle == Lower(info.path) || needle == "path:" + Lower(info.path))) {
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
  const DeviceIdentity identity = ResolveDeviceIdentity(fresh);
  const std::string& key = identity.key;
  const std::string model = DeviceModelKey(fresh);
  auto take = [&](uint32_t ordinal) {
    // Anyone waiting for this player has lost it. A serial or a port names one
    // pad, so its other reservation is spent too; a GUID may stand for an
    // identical pad still away, whose reservation stays.
    const bool unique = identity.unique;
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
  // 2b. A pad without a serial that came back in another port (or whose port
  // could not be read this time) falls back to its model, as before ports were
  // known: the oldest player an identical serial-less pad left behind.
  if (identity.source != IdentitySource::kSerial) {
    for (const auto& r : reserved_) {
      if (!r.serial && r.model == model && r.ordinal >= first && r.ordinal < kMaxGuestUsers &&
          !taken(r.ordinal)) {
        return take(r.ordinal);
      }
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
  const DeviceIdentity identity = ResolveDeviceIdentity(gone);
  // Two identical pads without serials or ports share a key; each keeps its
  // own player.
  if (std::any_of(reserved_.begin(), reserved_.end(), [&](const Reservation& r) {
        return r.key == identity.key && r.ordinal == gone.ordinal;
      })) {
    return;
  }
  reserved_.push_back({identity.key, DeviceModelKey(gone),
                       identity.source == IdentitySource::kSerial, gone.ordinal});
}

std::vector<std::pair<std::string, uint32_t>> SlotTable::Reservations() const {
  std::vector<std::pair<std::string, uint32_t>> out;
  for (const auto& r : reserved_) {
    out.emplace_back(r.key, r.ordinal);
  }
  return out;
}

}  // namespace rex::input
