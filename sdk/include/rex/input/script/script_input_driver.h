#pragma once
/**
 * @file        rex/input/script/script_input_driver.h
 * @brief       Scripted virtual pads, so split screen can be tested without
 *              controllers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>

namespace rex::input::script {

/// One entry of a pad script: "25:a" presses A at 25 s for a quarter second,
/// "60-90:stick_up+look_right" holds both from 60 s to 90 s, "100-110:disconnect"
/// unplugs the pad for those ten seconds.
///
/// "wait_event <name> [timeout_s]" and "wait_log <text> [timeout_s]" pause the
/// script once the steps before them are done, until the game signals <name>
/// (rex::input::SignalScriptEvent) or logs a line containing <text>. Times of
/// later steps count from the end of the wait. A wait gives up after its
/// timeout (default kDefaultWaitTimeout) with a warning, and the script goes
/// on. "30:wait_event x" starts waiting no sooner than 30 s.
struct Step {
  enum class Wait { kNone, kEvent, kLog };

  double start = 0;
  double end = 0;
  uint32_t controls = 0;  // kControl* bits
  bool disconnect = false;
  Wait wait = Wait::kNone;
  std::string target;  // event name or log text
  double timeout = 0;
  uint32_t segment = 0;  // waits before this step
};

constexpr double kDefaultWaitTimeout = 120.0;

// Low 16 bits are X_INPUT_GAMEPAD buttons; the rest are triggers and stick
// directions.
constexpr uint32_t kControlLT = 1u << 16;
constexpr uint32_t kControlRT = 1u << 17;
constexpr uint32_t kControlMoveUp = 1u << 18;
constexpr uint32_t kControlMoveDown = 1u << 19;
constexpr uint32_t kControlMoveRight = 1u << 20;
constexpr uint32_t kControlMoveLeft = 1u << 21;
constexpr uint32_t kControlLookUp = 1u << 22;
constexpr uint32_t kControlLookDown = 1u << 23;
constexpr uint32_t kControlLookRight = 1u << 24;
constexpr uint32_t kControlLookLeft = 1u << 25;

/// Steps separated by ',', ';', new lines or spaces. A wait step runs to the
/// next ',', ';' or new line, so its log text may hold spaces; put text with
/// those separators in double quotes. Bad steps are skipped and described in
/// *error.
std::vector<Step> ParseScript(std::string_view text, std::string* error = nullptr);

/// A script file: "key = value" lines, '#' comments. pad1..pad4 (or p1..p4)
/// hold each pad's script, "pads" the number of pads, "look_speed" the right
/// stick tilt in percent. The bo2 harness keys (BO2ZM_TEST_BUTTONS,
/// BO2ZM_TEST_BUTTONS_P2.._P4, players) are read too, so a .route file works
/// as is. Other keys are ignored.
struct ScriptFile {
  std::array<std::string, kMaxGuestUsers> pads;
  int pad_count = -1;   // -1: not given
  int look_speed = -1;  // -1: not given
};
ScriptFile ParseScriptFile(std::string_view text);

struct Frame {
  bool connected = true;
  uint32_t controls = 0;
};
/// The pad at `seconds` after the start of `segment` (the part of the script
/// after that many waits).
Frame Evaluate(const std::vector<Step>& steps, double seconds, uint32_t segment = 0);

/// Fills buttons, triggers and sticks. Moves tilt fully, looks by look_percent.
void ControlsToGamepad(uint32_t controls, int look_percent, X_INPUT_GAMEPAD* out);

/// Pads 1..N on players 1..N, each replaying its script.
class ScriptInputDriver final : public InputDriver {
 public:
  using Clock = std::function<double()>;  // seconds since start

  ScriptInputDriver(std::vector<std::vector<Step>> scripts, int look_percent,
                    Clock clock = nullptr);

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  /// Device ids of script pads: kScriptPadBase + pad index.
  static constexpr uint64_t kScriptPadBase = 0x5343525000000000ull;  // "SCRP"

 private:
  struct Pad {
    std::vector<Step> steps;
    std::vector<size_t> waits;  // indexes of wait steps, in order
    uint32_t segment = 0;
    double segment_start = 0;
    bool waiting = false;
    double wait_start = 0;
    std::map<std::string, uint64_t, std::less<>> consumed;  // per event key
    bool connected = false;
    bool seen = false;
    uint32_t controls = 0;
    uint32_t packet = 0;
    double last_seconds = -1;
    std::deque<X_INPUT_KEYSTROKE> keys;
  };
  /// Advances one pad to now. Caller holds mutex_. Returns connected.
  bool Update(size_t index);
  /// Moves past finished segments and waits. Returns seconds into the
  /// current segment.
  double Advance(size_t index, double now);
  Pad* PadFor(DeviceId id, size_t* index);

  std::vector<Pad> pads_;
  int look_percent_;
  Clock clock_;
  std::mutex mutex_;
};

}  // namespace rex::input::script
