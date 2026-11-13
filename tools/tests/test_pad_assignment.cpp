// Authored controller-routing cases; no runtime, game, or physical controller needed.
#include "../../core/bo2zm_test_pad_assignment.h"

#include <cassert>

using namespace rex::input;
using namespace bo2zm::test_pad;

int main() {
  const DeviceId physical0{10}, physical1{11}, physical2{13}, physical3{14}, keyboard{12};
  ScriptPadAssignment assignment;
  std::vector<DeviceId> result;
  const auto expect = [&](uint32_t user, std::initializer_list<DeviceId> expected) {
    assignment.DevicesForUser(user, result);
    assert(result == std::vector<DeviceId>(expected));
  };
  std::vector<DeviceInfo> devices{
      {keyboard, UINT32_MAX, "Keyboard", "", true},
      {kFourthScriptPad, UINT32_MAX, "Script 4", "", true},
      {physical0, 0, "First physical pad", "", false},
      {physical1, 1, "Second physical pad", "", false},
      {physical2, 2, "Third physical pad", "", false},
      {physical3, 3, "Fourth physical pad", "", false},
      {kFirstScriptPad, UINT32_MAX, "Script 1", "", true},
      {kSecondScriptPad, UINT32_MAX, "Script 2", "", true},
      {kThirdScriptPad, UINT32_MAX, "Script 3", "", true},
  };
  assignment.OnDevicesChanged(devices);
  expect(0, {keyboard, physical0, kFirstScriptPad});
  expect(1, {physical1, kSecondScriptPad});
  expect(2, {physical2, kThirdScriptPad});
  expect(3, {physical3, kFourthScriptPad});
  expect(4, {});
  expect(UINT32_MAX, {});

  // Disconnecting player one's physical pad must not promote player two.
  std::erase_if(devices, [&](const auto& device) { return device.id == physical0; });
  assignment.OnDevicesChanged(devices);
  expect(0, {keyboard, kFirstScriptPad});
  expect(1, {physical1, kSecondScriptPad});

  // Removing/reconnecting the scripted device affects only its guest slot.
  std::erase_if(devices, [&](const auto& device) { return device.id == kSecondScriptPad; });
  assignment.OnDevicesChanged(devices);
  expect(0, {keyboard, kFirstScriptPad});
  expect(1, {physical1});
  devices.push_back({kSecondScriptPad, UINT32_MAX, "Script 2", "", true});
  assignment.OnDevicesChanged(devices);
  expect(1, {physical1, kSecondScriptPad});

  // Secondary scripts can disconnect independently; surviving scripts keep
  // their exact ports and never leak into the synthetic user-zero fallback.
  for (uint32_t user = 1; user < kScriptPads.size(); ++user) {
    std::erase_if(devices, [&](const auto& device) { return device.id == kScriptPads[user]; });
  }
  assignment.OnDevicesChanged(devices);
  expect(0, {keyboard, kFirstScriptPad});
  expect(1, {physical1});
  expect(2, {physical2});
  expect(3, {physical3});
  devices.insert(devices.begin(), {kFourthScriptPad, UINT32_MAX, "Script 4", "", true});
  assignment.OnDevicesChanged(devices);
  expect(0, {keyboard, kFirstScriptPad});
  expect(1, {physical1});
  expect(2, {physical2});
  expect(3, {physical3, kFourthScriptPad});

  assignment.OnDevicesChanged({});
  expect(0, {});
  expect(1, {});
  expect(2, {});
  expect(3, {});
}
