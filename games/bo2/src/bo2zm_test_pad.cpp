// Test helper: a virtual gamepad driven by a button script, so PC test runs can
// reach a match unattended (each native renderer test needs Survival: Town).
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
//   --bo2zm_test_buttons="25:down,26:down,27:a,35:a"
//
// "T:button" presses for 0.25 s, "T1-T2:button" holds; seconds from when the
// input system is created. Buttons: a b x y start back up down left right lb rb
// lt rt ls rs (stick clicks), stick_up stick_down stick_left stick_right (left stick: move) and
// look_up look_down look_left look_right (right stick: camera, e.g.
// "60-75:look_right" turns in place for test captures). The pad is added to the SDK
// input system as a synthetic device of user 0, beside the real pad and
// keyboard. Optional _p2, _p3 and _p4 scripts feed their own guest ports. Empty
// defaults add nothing; a secondary script also enables an idle primary pad.
// Any player's script accepts "60-65:disconnect": that script pad disappears
// for [60, 65) seconds, then returns with the same identity. Other devices stay
// connected. Disconnect requires an explicit finite interval and suppresses
// buttons during it; expired presses are not replayed on reconnect.

#include "bo2zm_test_pad.h"
#include "bo2zm_test_pad_assignment.h"

#include "bo2zm_benchmark.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(bo2zm_test_buttons, "", "BO2",
                      "Test helper: virtual pad script, e.g. \"25:down,27:a,60-65:disconnect\" (seconds from startup)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_test_buttons_p2, "", "BO2",
                      "Test helper: independent second-player pad script; empty disables it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_test_buttons_p3, "", "BO2",
                      "Test helper: independent third-player pad script; empty disables it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_test_buttons_p4, "", "BO2",
                      "Test helper: independent fourth-player pad script; empty disables it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_test_look_speed, 35, "BO2",
                     "Test helper: right-stick tilt of the look_* buttons, in percent (lower turns slower)")
    .range(1, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::test_pad {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
namespace in = rex::input;

constexpr double kPress = 0.25;  // seconds of a single press

// X_INPUT_GAMEPAD bits; the triggers in bits 16-17, the left stick in 18-21, the right stick in 22-25. The key
// is the XInputGetKeystroke VK_PAD code, for menus that read keystrokes.
struct Button {
  std::string_view name;
  uint32_t bits;
  uint16_t key;
};
constexpr Button kButtons[] = {
    {"a", 0x1000, 0x5800},          {"b", 0x2000, 0x5801},
    {"x", 0x4000, 0x5802},          {"y", 0x8000, 0x5803},
    {"rb", 0x0200, 0x5804},         {"lb", 0x0100, 0x5805},
    {"lt", 1u << 16, 0x5806},       {"rt", 1u << 17, 0x5807},
    {"up", 0x0001, 0x5810},         {"down", 0x0002, 0x5811},
    {"left", 0x0004, 0x5812},       {"right", 0x0008, 0x5813},
    {"start", 0x0010, 0x5814},      {"back", 0x0020, 0x5815},
    {"ls", 0x0040, 0x5816},         {"rs", 0x0080, 0x5817},
    {"stick_up", 1u << 18, 0x5820}, {"stick_down", 1u << 19, 0x5821},
    {"stick_right", 1u << 20, 0x5822}, {"stick_left", 1u << 21, 0x5823},
    {"look_up", 1u << 22, 0x5830},  {"look_down", 1u << 23, 0x5831},
    {"look_right", 1u << 24, 0x5832}, {"look_left", 1u << 25, 0x5833},
};

struct Step {
  double start;
  double end;
  uint32_t bits;
  bool disconnect;
};

bool Number(std::string_view text, double& value) {
  const std::string copy(text);
  char* end = nullptr;
  value = std::strtod(copy.c_str(), &end);
  return !copy.empty() && end == copy.c_str() + copy.size() && std::isfinite(value) && value >= 0.0;
}

std::vector<Step> Parse(std::string_view text) {
  std::vector<Step> steps;
  size_t i = 0;
  const auto separator = [](char c) { return c == ' ' || c == ',' || c == ';'; };
  while (i < text.size()) {
    while (i < text.size() && separator(text[i])) {
      ++i;
    }
    size_t j = i;
    while (j < text.size() && !separator(text[j])) {
      ++j;
    }
    if (j == i) {
      break;
    }
    const std::string_view step = text.substr(i, j - i);
    i = j;
    const size_t colon = step.find(':');
    const std::string_view times = step.substr(0, colon);
    const std::string_view name = colon == std::string_view::npos ? std::string_view() : step.substr(colon + 1);
    const Button* button = nullptr;
    for (const Button& b : kButtons) {
      if (b.name == name) {
        button = &b;
      }
    }
    const size_t dash = times.find('-');
    const bool disconnect = name == "disconnect" && dash != std::string_view::npos;
    double start = 0.0;
    double end = 0.0;
    const bool ok = (button || disconnect) && Number(times.substr(0, dash), start) &&
                    (dash == std::string_view::npos ? (end = start + kPress, true)
                                                    : (Number(times.substr(dash + 1), end) && end > start));
    if (!ok) {
      REXLOG_WARN("[test pad] invalid step \"{}\": ignored", step);
      continue;
    }
    steps.push_back({start, end, button ? button->bits : 0, disconnect});
  }
  return steps;
}

class ScriptPad final : public in::InputDriver {
 public:
  using Now = Clock::time_point (*)();
  explicit ScriptPad(std::vector<Step> steps, uint32_t user = 0, Now now = Clock::now)
      : InputDriver(nullptr, 0), user_(user), id_(kScriptPads[user]),
        steps_(std::move(steps)), now_(now), start_(now_()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    std::lock_guard lock(mutex_);
    if (!Update()) return;
    in::DeviceInfo info;
    info.id = id_;
    info.name = user_ ? "Test script pad " + std::to_string(user_ + 1) : "Test script pad";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != id_) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::lock_guard lock(mutex_);
    if (!Update()) return X_ERROR_DEVICE_NOT_CONNECTED;
    const uint32_t bits = current_;
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_;
      out_state->gamepad.buttons = uint16_t(bits & 0xFFFF);
      // The benchmark's route sets its own tilts while it runs (bo2zm_benchmark.cpp: a small circle in place).
      static const int look_usual = REXCVAR_GET(bo2zm_test_look_speed);
      int move_percent = 100, look_percent = look_usual;
      if (!user_) bo2zm::benchmark::PathTilts(move_percent, look_percent);
      const int16_t move = int16_t(30000 * move_percent / 100);
      int16_t lx = 0, ly = 0;
      if ((bits >> 18) & 0x1) ly = move;
      if ((bits >> 19) & 0x1) ly = int16_t(-move);
      if ((bits >> 20) & 0x1) lx = move;
      if ((bits >> 21) & 0x1) lx = int16_t(-move);
      out_state->gamepad.thumb_lx = lx;
      out_state->gamepad.thumb_ly = ly;
      const int16_t look = int16_t(32767 * look_percent / 100);
      int16_t rx = 0, ry = 0;
      if ((bits >> 22) & 0x1) ry = look;
      if ((bits >> 23) & 0x1) ry = int16_t(-look);
      if ((bits >> 24) & 0x1) rx = look;
      if ((bits >> 25) & 0x1) rx = int16_t(-look);
      out_state->gamepad.thumb_rx = rx;
      out_state->gamepad.thumb_ry = ry;
      out_state->gamepad.left_trigger = ((bits >> 16) & 0x1) ? 0xFF : 0;
      out_state->gamepad.right_trigger = ((bits >> 17) & 0x1) ? 0xFF : 0;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t flags, in::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != id_) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::lock_guard lock(mutex_);
    if (!Update()) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      // Like the SDK's empty pad (nop_input_driver.cpp).
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    if (id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(mutex_);
    return Update() ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t flags, in::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (id != id_) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::lock_guard lock(mutex_);
    if (!Update()) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (keys_.empty()) {
      return X_ERROR_EMPTY;
    }
    if (out_keystroke) {
      *out_keystroke = keys_.front();
    }
    keys_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  // Caller holds mutex_. Every query samples elapsed wall time, including
  // enumeration and queries that do not consume button state.
  bool Update() {
    const double t = std::chrono::duration<double>(now_() - start_).count();
    bool connected = true, crossed_disconnect = false;
    uint32_t bits = 0;
    for (const Step& s : steps_) {
      if (s.disconnect) {
        if (t >= s.start && t < s.end) connected = false;
        if (last_t_ < s.start && t >= s.start) crossed_disconnect = true;
        continue;
      }
      if (t >= s.start && t < s.end) {
        bits |= s.bits;
      }
    }
    last_t_ = t;
    if (crossed_disconnect || connected != connected_) {
      // Forget queued keys even if polling skipped the entire unplug window.
      // Removal releases the old state. A hold still active on reconnect gets
      // a fresh keydown; no disconnected-device keycodes or stale keyups escape.
      current_ = 0;
      keys_.clear();
      ++packet_;
      if (connected != connected_) {
        REXLOG_INFO("[test pad] player {}: {} at {:.1f} s", user_ + 1,
                    connected ? "connected" : "disconnected", t);
      }
      connected_ = connected;
    }
    if (!connected_) return false;
    if (!user_) bits |= bo2zm::benchmark::PathButtons();
    if (bits != current_) {
      for (const Button& b : kButtons) {
        const bool before = (current_ & b.bits) != 0;
        const bool now = (bits & b.bits) != 0;
        if (before == now) {
          continue;
        }
        in::X_INPUT_KEYSTROKE key{};
        key.virtual_key = b.key;
        key.user_index = static_cast<uint8_t>(user_);
        key.flags = uint16_t(now ? in::X_INPUT_KEYSTROKE_KEYDOWN : in::X_INPUT_KEYSTROKE_KEYUP);
        if (keys_.size() < 64) {
          keys_.push_back(key);
        }
        REXLOG_INFO("[test pad] player {}: {} {} at {:.1f} s", user_ + 1, b.name, now ? "down" : "up", t);
      }
      current_ = bits;
      ++packet_;
    }
    return true;
  }

  const uint32_t user_;
  const in::DeviceId id_;
  const std::vector<Step> steps_;
  const Now now_;
  const Clock::time_point start_;
  std::mutex mutex_;
  double last_t_ = -1.0;
  bool connected_ = true;
  uint32_t current_ = 0;
  uint32_t packet_ = 1;
  std::deque<in::X_INPUT_KEYSTROKE> keys_;
};

}  // namespace

void WrapInput(rex::RuntimeConfig& config) {
  const std::array<std::string, in::kMaxGuestUsers> scripts{
      REXCVAR_GET(bo2zm_test_buttons), REXCVAR_GET(bo2zm_test_buttons_p2),
      REXCVAR_GET(bo2zm_test_buttons_p3), REXCVAR_GET(bo2zm_test_buttons_p4)};
  const bool benchmark = bo2zm::benchmark::IsActive();
  // bo2zm_benchmark drives its route through this pad too, with or without a script.
  if ((std::all_of(scripts.begin(), scripts.end(), [](const auto& script) { return script.empty(); }) &&
       !benchmark) || !config.input_factory) {
    return;
  }
  std::array<std::vector<Step>, in::kMaxGuestUsers> steps;
  bool any_steps = false;
  for (size_t user = 0; user < scripts.size(); ++user) {
    steps[user] = Parse(scripts[user]);
    any_steps |= !steps[user].empty();
    if (steps[user].empty() && !scripts[user].empty()) {
      REXLOG_WARN("[test pad] player {} script has no valid steps", user + 1);
    }
  }
  if (!any_steps && !benchmark) return;
  auto base = config.input_factory;
  config.input_factory = [base, steps](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    if (system && !tool_mode) {
      // The default factory returns a rex::input::InputSystem (ui/rex_app.cpp).
      auto* input = static_cast<in::InputSystem*>(system.get());
      input->AddDriver(std::make_unique<ScriptPad>(steps[0]));
      bool secondary = false;
      for (uint32_t user = 1; user < steps.size(); ++user) {
        if (steps[user].empty()) continue;
        input->AddDriver(std::make_unique<ScriptPad>(steps[user], user));
        secondary = true;
        REXLOG_INFO("[test pad] player {} script pad with {} steps", user + 1, steps[user].size());
      }
      if (secondary) input->SetDeviceAssignment(std::make_unique<ScriptPadAssignment>());
      REXLOG_INFO("[test pad] script pad with {} steps", steps[0].size());
    }
    return system;
  };
}

}  // namespace bo2zm::test_pad
