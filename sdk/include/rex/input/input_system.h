#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/slot_table.h>
#include <rex/input/state_merge.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const;

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Replaces any previous assignment after in-flight queries finish.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  /// True once after a controller connected or disconnected (or moved to another player), so the
  /// caller can tell the guest (XN_SYS_INPUTDEVICESCHANGED) outside the input lock.
  bool TakeDevicesChanged() { return devices_changed_.exchange(false, std::memory_order_acq_rel); }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

  /// One device as the guest sees it, for diagnostics (rex-padtest, the state
  /// log).
  struct DeviceSnapshot {
    DeviceInfo info;
    int32_t user = -1;  // guest user it feeds, -1 for none
    X_INPUT_STATE state = {};
    bool has_state = false;
    X_INPUT_CAPABILITIES caps = {};
    bool has_caps = false;
  };
  /// Re-enumerates and reads every device's raw state (no deadzones, no UI
  /// blockers).
  std::vector<DeviceSnapshot> SnapshotDevices();

  /// With input_log_state on, logs every pad's state at most every 0.5 s.
  /// GetState calls it; tools that only take snapshots call it themselves.
  void LogStateIfDue();
  /// Guest user a device feeds, or -1.
  int32_t UserForDevice(DeviceId id) const;
  /// Rumbles one device directly, bypassing the guest user mapping.
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration);
  const SlotTable& slot_table() const { return slot_table_; }

 private:
  // Queries hold mutex_ through driver calls: cache entries, assignment and
  // active state must belong to the same device generation. Polling driver
  // callbacks, active callbacks and assignment policies must not re-enter
  // InputSystem.
  // Lifecycle operations take lifecycle_mutex_ before mutex_. Attachment and
  // destruction release mutex_ before dispatching to the UI thread; queries
  // return disconnected while those operations have quiesced the drivers.
  // UI callbacks dispatched by attachment/destruction may query input, but
  // must not call lifecycle mutators (Setup, Shutdown, AddDriver, AttachWindow,
  // SetActiveCallback or SetDeviceAssignment) while the caller waits for UI.
  // Both locks outlive the state they protect.
  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  bool accepting_queries_ = true;
  std::atomic<bool> devices_changed_{false};

  // Caller holds mutex_ for these helpers.
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  /// Reads the slot cvars into slot_table_.
  void ApplySlotConfig();
  /// UserForDevice without taking mutex_.
  int32_t UserForDeviceLocked(DeviceId id) const;
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;

  // Switch layer: when the controllers were last enumerated, in steady_clock nanoseconds. RefreshDevices used
  // to be called on every GetState, SetState and GetCapabilities from the game (several per frame),
  // allocating memory and querying every driver each time. See RefreshDevices.
  int64_t last_refresh_ns_ = 0;

  SlotTable slot_table_;
  std::string applied_pins_text_;
  std::set<DeviceId> ignored_virtual_;
  bool steam_virtual_warned_ = false;
  std::atomic<int64_t> last_state_log_ms_{0};
};

/// The scripted virtual pads the hid_virtual_pad* cvars ask for, or null.
std::unique_ptr<InputDriver> CreateScriptPadDriverFromConfig();

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
