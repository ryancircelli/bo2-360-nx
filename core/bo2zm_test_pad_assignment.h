// Route optional scripted pads without changing physical controller slots.
#pragma once

#include <algorithm>
#include <array>
#include <rex/input/device_assignment.h>

namespace bo2zm::test_pad {

inline constexpr rex::input::DeviceId kFirstScriptPad{0x424F325A4D504144ull};
inline constexpr rex::input::DeviceId kSecondScriptPad{0x424F325A4D504145ull};
inline constexpr rex::input::DeviceId kThirdScriptPad{0x424F325A4D504146ull};
inline constexpr rex::input::DeviceId kFourthScriptPad{0x424F325A4D504147ull};
inline constexpr std::array<rex::input::DeviceId, rex::input::kMaxGuestUsers> kScriptPads{
    kFirstScriptPad, kSecondScriptPad, kThirdScriptPad, kFourthScriptPad};

class ScriptPadAssignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    usual_.OnDevicesChanged(devices);
    connected_.fill(false);
    for (const auto& device : devices) {
      const auto found = std::find(kScriptPads.begin(), kScriptPads.end(), device.id);
      if (found != kScriptPads.end()) connected_[found - kScriptPads.begin()] = true;
    }
  }

  void DevicesForUser(uint32_t user, std::vector<rex::input::DeviceId>& out) const override {
    usual_.DevicesForUser(user, out);
    // SlotAssignment parks synthetic devices on user zero. Keep its P1 order,
    // but move each secondary script to its fixed guest port, even with gaps.
    std::erase_if(out, [](auto id) {
      return std::find(kScriptPads.begin() + 1, kScriptPads.end(), id) != kScriptPads.end();
    });
    if (user > 0 && user < kScriptPads.size() && connected_[user]) {
      out.push_back(kScriptPads[user]);
    }
  }

 private:
  rex::input::SlotAssignment usual_;
  std::array<bool, rex::input::kMaxGuestUsers> connected_{};
};

}  // namespace bo2zm::test_pad
