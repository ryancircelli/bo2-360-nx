/**
 * @file        input/script/script_input_driver.cpp
 * @brief       Scripted virtual pads, so split screen can be tested without
 *              controllers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/script/script_input_driver.h>
#include <rex/input/script/script_events.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include <fmt/format.h>

#include <rex/logging.h>

namespace rex::input::script {

namespace {

struct Control {
  std::string_view name;
  uint32_t bits;
  uint16_t key;  // VK_PAD_* for XInputGetKeystroke
};

constexpr Control kControls[] = {
    {"a", X_INPUT_GAMEPAD_A, 0x5800},
    {"b", X_INPUT_GAMEPAD_B, 0x5801},
    {"x", X_INPUT_GAMEPAD_X, 0x5802},
    {"y", X_INPUT_GAMEPAD_Y, 0x5803},
    {"rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0x5804},
    {"lb", X_INPUT_GAMEPAD_LEFT_SHOULDER, 0x5805},
    {"lt", kControlLT, 0x5806},
    {"rt", kControlRT, 0x5807},
    {"up", X_INPUT_GAMEPAD_DPAD_UP, 0x5810},
    {"down", X_INPUT_GAMEPAD_DPAD_DOWN, 0x5811},
    {"left", X_INPUT_GAMEPAD_DPAD_LEFT, 0x5812},
    {"right", X_INPUT_GAMEPAD_DPAD_RIGHT, 0x5813},
    {"start", X_INPUT_GAMEPAD_START, 0x5814},
    {"back", X_INPUT_GAMEPAD_BACK, 0x5815},
    {"ls", X_INPUT_GAMEPAD_LEFT_THUMB, 0x5816},
    {"rs", X_INPUT_GAMEPAD_RIGHT_THUMB, 0x5817},
    {"stick_up", kControlMoveUp, 0x5820},
    {"stick_down", kControlMoveDown, 0x5821},
    {"stick_right", kControlMoveRight, 0x5822},
    {"stick_left", kControlMoveLeft, 0x5823},
    {"look_up", kControlLookUp, 0x5830},
    {"look_down", kControlLookDown, 0x5831},
    {"look_right", kControlLookRight, 0x5832},
    {"look_left", kControlLookLeft, 0x5833},
    {"guide", X_INPUT_GAMEPAD_GUIDE, 0},  // no keystroke: the Guide overlay takes it
};

constexpr double kTapSeconds = 0.25;
constexpr int16_t kFullTilt = 30000;

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

bool Seconds(std::string_view text, double& out) {
  const std::string copy(text);
  if (copy.empty()) {
    return false;
  }
  char* end = nullptr;
  out = std::strtod(copy.c_str(), &end);
  return end == copy.c_str() + copy.size() && std::isfinite(out) && out >= 0;
}

bool ControlBits(std::string_view names, uint32_t& bits, bool& disconnect) {
  bits = 0;
  disconnect = false;
  size_t start = 0;
  while (start <= names.size()) {
    size_t end = names.find('+', start);
    if (end == std::string_view::npos) {
      end = names.size();
    }
    const std::string name = Lower(names.substr(start, end - start));
    start = end + 1;
    if (name == "disconnect") {
      disconnect = true;
      continue;
    }
    auto it = std::find_if(std::begin(kControls), std::end(kControls),
                           [&](const Control& c) { return c.name == name; });
    if (it == std::end(kControls)) {
      return false;
    }
    bits |= it->bits;
  }
  // "disconnect" stands alone.
  return disconnect ? bits == 0 : bits != 0;
}

int ParseInt(std::string_view text) {
  const std::string copy(Trim(text));
  char* end = nullptr;
  const long value = std::strtol(copy.c_str(), &end, 10);
  return !copy.empty() && end == copy.c_str() + copy.size() ? static_cast<int>(value) : -1;
}

}  // namespace

namespace {

// "[<s>:]wait_event <name> [timeout]" or "[<s>:]wait_log <text> [timeout]".
// Returns false if `entry` is not a wait; sets ok when it parsed.
bool ParseWait(std::string_view entry, Step& step, bool& ok) {
  ok = false;
  std::string_view rest = Trim(entry);
  double start = 0;
  const size_t colon = rest.find(':');
  size_t wait_at = rest.find("wait_");
  const size_t until_at = rest.find("until_log");
  if (until_at != std::string_view::npos &&
      (wait_at == std::string_view::npos || until_at < wait_at)) {
    wait_at = until_at;
  }
  if (wait_at == std::string_view::npos) {
    return false;
  }
  if (colon != std::string_view::npos && colon < wait_at) {
    if (Trim(rest.substr(colon + 1, wait_at - colon - 1)).size() != 0 ||
        !Seconds(Trim(rest.substr(0, colon)), start)) {
      return false;
    }
    rest = rest.substr(wait_at);
  } else if (wait_at != 0) {
    return false;
  }
  Step::Wait kind;
  bool interrupt = false;
  if (rest.starts_with("until_log")) {
    kind = Step::Wait::kLog;
    interrupt = true;
    rest.remove_prefix(9);
  } else if (rest.starts_with("wait_event")) {
    kind = Step::Wait::kEvent;
    rest.remove_prefix(10);
  } else if (rest.starts_with("wait_log")) {
    kind = Step::Wait::kLog;
    rest.remove_prefix(8);
  } else {
    return false;
  }
  if (!rest.empty() && !std::isspace(static_cast<unsigned char>(rest.front()))) {
    return false;  // e.g. "wait_events"
  }
  rest = Trim(rest);
  std::string target;
  double timeout = kDefaultWaitTimeout;
  if (!rest.empty() && rest.front() == '"') {
    const size_t close = rest.find('"', 1);
    if (close == std::string_view::npos) {
      return true;
    }
    target = std::string(rest.substr(1, close - 1));
    const std::string_view tail = Trim(rest.substr(close + 1));
    if (!tail.empty() && !Seconds(tail, timeout)) {
      return true;
    }
  } else {
    // A last word that is a number is the timeout.
    const size_t space = rest.find_last_of(" \t");
    double value = 0;
    if (space != std::string_view::npos && Seconds(Trim(rest.substr(space + 1)), value)) {
      timeout = value;
      rest = Trim(rest.substr(0, space));
    }
    target = std::string(rest);
  }
  if (target.empty() || (kind == Step::Wait::kEvent &&
                         target.find_first_of(" \t") != std::string::npos)) {
    return true;
  }
  step = {};
  step.wait = kind;
  step.interrupt = interrupt;
  step.target = std::move(target);
  step.timeout = timeout;
  step.start = start;
  step.end = start;
  ok = true;
  return true;
}

// Splits on ',', ';' and new lines, keeping double-quoted text whole.
std::vector<std::string_view> SplitPieces(std::string_view text) {
  std::vector<std::string_view> pieces;
  bool quoted = false;
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); i++) {
    const char c = i < text.size() ? text[i] : ',';
    if (c == '"') {
      quoted = !quoted;
    } else if (!quoted && (c == ',' || c == ';' || c == '\n' || c == '\r')) {
      pieces.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  return pieces;
}

bool ParseTimed(std::string_view entry, Step& step) {
  step = {};
  const size_t colon = entry.find(':');
  const std::string_view times = entry.substr(0, colon);
  const size_t dash = times.find('-');
  bool ok = colon != std::string_view::npos &&
            ControlBits(entry.substr(colon + 1), step.controls, step.disconnect) &&
            Seconds(times.substr(0, dash), step.start);
  if (ok) {
    if (dash == std::string_view::npos) {
      // A tap. A disconnect needs an explicit end.
      step.end = step.start + kTapSeconds;
      ok = !step.disconnect;
    } else {
      ok = Seconds(times.substr(dash + 1), step.end) && step.end > step.start;
    }
  }
  return ok;
}

}  // namespace

std::vector<Step> ParseScript(std::string_view text, std::string* error) {
  std::vector<Step> steps;
  std::string problems;
  uint32_t segment = 0;
  auto bad = [&](std::string_view entry) {
    problems += fmt::format("{}\"{}\"", problems.empty() ? "" : ", ", Trim(entry));
  };
  for (const std::string_view piece : SplitPieces(text)) {
    if (Trim(piece).empty()) {
      continue;
    }
    Step step;
    bool ok = false;
    if (ParseWait(piece, step, ok)) {
      if (!ok) {
        bad(piece);
        continue;
      }
      step.segment = segment++;
      steps.push_back(std::move(step));
      continue;
    }
    // Timed steps, separated by spaces too.
    size_t i = 0;
    while (i < piece.size()) {
      while (i < piece.size() && std::isspace(static_cast<unsigned char>(piece[i]))) {
        i++;
      }
      size_t j = i;
      while (j < piece.size() && !std::isspace(static_cast<unsigned char>(piece[j]))) {
        j++;
      }
      if (i == j) {
        break;
      }
      const std::string_view entry = piece.substr(i, j - i);
      i = j;
      if (!ParseTimed(entry, step)) {
        bad(entry);
        continue;
      }
      step.segment = segment;
      steps.push_back(step);
    }
  }
  if (error) {
    *error = problems.empty()
                 ? std::string()
                 : "skipped " + problems +
                       " (want <s>:<control> or <s>-<s>:<control>[+<control>...], "
                       "<s>-<s>:disconnect, wait_event <name> [timeout_s], or "
                       "wait_log <text> [timeout_s])";
  }
  return steps;
}

ScriptFile ParseScriptFile(std::string_view text) {
  ScriptFile file;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = Trim(text.substr(start, end - start));
    start = end + 1;
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string key = Lower(Trim(line.substr(0, eq)));
    const std::string_view value = Trim(line.substr(eq + 1));
    int pad = -1;
    if (key == "pad1" || key == "p1" || key == "bo2zm_test_buttons") {
      pad = 0;
    } else if (key.size() == 4 && key.starts_with("pad") && key[3] >= '2' && key[3] <= '4') {
      pad = key[3] - '1';
    } else if (key.size() == 2 && key[0] == 'p' && key[1] >= '2' && key[1] <= '4') {
      pad = key[1] - '1';
    } else if (key.starts_with("bo2zm_test_buttons_p") && key.size() == 21 && key[20] >= '2' &&
               key[20] <= '4') {
      pad = key[20] - '1';
    } else if (key == "pads" || key == "players") {
      file.pad_count = ParseInt(value);
    } else if (key == "look_speed" || key == "bo2zm_test_look_speed") {
      file.look_speed = ParseInt(value);
    }
    if (pad >= 0) {
      file.pads[pad] = std::string(value);
    }
  }
  return file;
}

Frame Evaluate(const std::vector<Step>& steps, double seconds, uint32_t segment) {
  Frame frame;
  for (const auto& step : steps) {
    if (step.wait != Step::Wait::kNone || step.segment != segment || seconds < step.start ||
        seconds >= step.end) {
      continue;
    }
    if (step.disconnect) {
      frame.connected = false;
    } else {
      frame.controls |= step.controls;
    }
  }
  if (!frame.connected) {
    frame.controls = 0;
  }
  return frame;
}

void ControlsToGamepad(uint32_t controls, int look_percent, X_INPUT_GAMEPAD* out) {
  *out = {};
  out->buttons = static_cast<uint16_t>(controls & 0xFFFF);
  out->left_trigger = controls & kControlLT ? 0xFF : 0;
  out->right_trigger = controls & kControlRT ? 0xFF : 0;
  auto axis = [](uint32_t controls, uint32_t plus, uint32_t minus, int16_t tilt) -> int16_t {
    return static_cast<int16_t>((controls & plus ? tilt : 0) - (controls & minus ? tilt : 0));
  };
  const auto look = static_cast<int16_t>(32767 * std::clamp(look_percent, 1, 100) / 100);
  out->thumb_lx = axis(controls, kControlMoveRight, kControlMoveLeft, kFullTilt);
  out->thumb_ly = axis(controls, kControlMoveUp, kControlMoveDown, kFullTilt);
  out->thumb_rx = axis(controls, kControlLookRight, kControlLookLeft, look);
  out->thumb_ry = axis(controls, kControlLookUp, kControlLookDown, look);
}

namespace {

// Live layer (LivePadCommand): what a running test adds on top of each script pad.
struct LiveTap {
  double start, end;
  uint32_t bits;
  bool seen;  // a tap lasts until one poll saw it, so a slow frame never drops it
};
struct LivePad {
  uint32_t held = 0;
  std::vector<LiveTap> taps;
  double queue_end = -1;  // end of the last queued tap (none yet: the first tap is not delayed)
  int16_t axis[4] = {};      // lx ly rx ry
  double axis_until[4] = {};  // 0 off, -1 until changed, else LiveNow() expiry
  uint8_t trig[2] = {};
  double trig_until[2] = {};
};
std::mutex g_live_mutex;
LivePad g_live[kMaxGuestUsers];
std::atomic<size_t> g_live_pads{0};

double LiveNow() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
bool LiveOn(double until, double now) { return until < 0 || (until > 0 && now < until); }

// Buttons the live layer holds now; analog overrides go to *analog with *mask (bits as Pad::live_mask).
uint32_t LiveOverlay(size_t index, X_INPUT_GAMEPAD* analog, uint8_t* mask) {
  std::lock_guard<std::mutex> lock(g_live_mutex);
  LivePad& p = g_live[index];
  const double now = LiveNow();
  std::erase_if(p.taps, [&](const LiveTap& t) { return t.seen && now >= t.end; });
  uint32_t bits = p.held;
  for (auto& t : p.taps) {
    if (now >= t.start) {
      bits |= t.bits;
      t.seen = true;
    }
  }
  *analog = {};
  *mask = 0;
  be<int16_t>* axes[4] = {&analog->thumb_lx, &analog->thumb_ly, &analog->thumb_rx, &analog->thumb_ry};
  for (int i = 0; i < 4; i++) {
    if (LiveOn(p.axis_until[i], now)) {
      *axes[i] = p.axis[i];
      *mask |= 1 << i;
    }
  }
  uint8_t* trig[2] = {&analog->left_trigger, &analog->right_trigger};
  for (int i = 0; i < 2; i++) {
    if (LiveOn(p.trig_until[i], now)) {
      *trig[i] = p.trig[i];
      *mask |= 16 << i;
      if (p.trig[i] >= 0x80) bits |= i ? kControlRT : kControlLT;  // keystroke for menus that read them
    }
  }
  return bits;
}

}  // namespace

ScriptInputDriver::ScriptInputDriver(std::vector<std::vector<Step>> scripts, int look_percent,
                                     Clock clock)
    : InputDriver(nullptr, 0), look_percent_(look_percent), clock_(std::move(clock)) {
  if (!clock_) {
    const auto start = std::chrono::steady_clock::now();
    clock_ = [start] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
  }
  scripts.resize(std::min<size_t>(scripts.size(), kMaxGuestUsers));
  for (auto& steps : scripts) {
    Pad pad;
    pad.steps = std::move(steps);
    for (size_t i = 0; i < pad.steps.size(); i++) {
      const Step& step = pad.steps[i];
      if (step.wait == Step::Wait::kNone) {
        continue;
      }
      pad.waits.push_back(i);
      if (step.wait == Step::Wait::kLog) {
        WatchLogText(step.target);
      }
    }
    pads_.push_back(std::move(pad));
  }
  g_live_pads = pads_.size();
}

namespace {

std::string EventKey(const Step& step) {
  return step.wait == Step::Wait::kLog ? LogEventName(step.target) : step.target;
}

std::string Describe(const Step& step) {
  return step.wait == Step::Wait::kLog ? fmt::format("a log line with \"{}\"", step.target)
                                       : fmt::format("event \"{}\"", step.target);
}

}  // namespace

double ScriptInputDriver::Advance(size_t index, double now) {
  Pad& pad = pads_[index];
  // The lines below name the log text; they must not count as seeing it.
  IgnoreLogLinesHere quiet;
  while (true) {
    if (pad.waiting) {
      const Step& wait = pad.steps[pad.waits[pad.segment]];
      const std::string key = EventKey(wait);
      uint64_t& used = pad.consumed[key];
      const bool fired = ScriptEventCount(key) > used;
      const bool timed_out = now - pad.wait_start >= wait.timeout;
      if (!fired && !timed_out) {
        return 0;
      }
      if (fired) {
        used++;
        REXLOG_INFO("Script pad {}: got {} after {:.1f} s; going on", index + 1, Describe(wait),
                    now - pad.wait_start);
      } else {
        REXLOG_WARN("Script pad {}: no {} within {:.0f} s; going on anyway", index + 1,
                    Describe(wait), wait.timeout);
      }
      pad.waiting = false;
      pad.segment++;
      pad.segment_start = now;
      pad.last_seconds = -1;
      continue;
    }
    if (pad.segment >= pad.waits.size()) {
      return now - pad.segment_start;
    }
    // The segment ends at its wait's own time or its last step, whichever is
    // later. An until_log wait ends it as soon as the log line shows up.
    const Step& wait = pad.steps[pad.waits[pad.segment]];
    double end = wait.start;
    if (wait.interrupt) {
      uint64_t& used = pad.consumed[EventKey(wait)];
      if (ScriptEventCount(EventKey(wait)) > used && now - pad.segment_start >= wait.start) {
        pad.waiting = true;
        pad.wait_start = now;
        continue;
      }
    }
    for (const auto& step : pad.steps) {
      if (step.wait == Step::Wait::kNone && step.segment == pad.segment) {
        end = std::max(end, step.end);
      }
    }
    if (now - pad.segment_start < end) {
      return now - pad.segment_start;
    }
    pad.waiting = true;
    pad.wait_start = pad.segment_start + end;
    REXLOG_INFO("Script pad {}: waiting for {} (up to {:.0f} s)", index + 1, Describe(wait),
                wait.timeout);
  }
}

bool ScriptInputDriver::Update(size_t index) {
  Pad& pad = pads_[index];
  const double now = Advance(index, clock_());
  const Frame frame = pad.waiting ? Frame{true, 0}
                                  : Evaluate(pad.steps, now, pad.segment);
  // A disconnect shorter than the gap between polls still counts.
  bool blinked = false;
  for (const auto& step : pad.steps) {
    if (step.disconnect && step.segment == pad.segment && !pad.waiting &&
        pad.last_seconds < step.start && now >= step.end) {
      blinked = true;
    }
  }
  pad.last_seconds = now;
  if (!pad.seen || frame.connected != pad.connected || blinked) {
    if (pad.seen) {
      REXLOG_INFO("Script pad {}: {} at {:.2f} s", index + 1,
                  frame.connected && !blinked ? "connected" : "disconnected", now);
    }
    pad.seen = true;
    pad.connected = frame.connected;
    pad.controls = 0;
    pad.keys.clear();
    pad.packet++;
    if (blinked) {
      // The pad left and came back between two polls: show it gone once, so
      // the game sees the unplug.
      pad.connected = false;
      return false;
    }
  }
  if (!pad.connected) {
    return false;
  }
  X_INPUT_GAMEPAD live;
  uint8_t live_mask = 0;
  const uint32_t controls = frame.controls | LiveOverlay(index, &live, &live_mask);
  if (live_mask != pad.live_mask || std::memcmp(&live, &pad.live, sizeof live) != 0) {
    pad.live = live;
    pad.live_mask = live_mask;
    pad.packet++;
  }
  if (controls != pad.controls) {
    for (const auto& control : kControls) {
      const bool was = pad.controls & control.bits;
      const bool is = controls & control.bits;
      if (was == is || control.key == 0) {
        continue;
      }
      X_INPUT_KEYSTROKE key = {};
      key.virtual_key = control.key;
      key.flags = static_cast<uint16_t>(is ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP);
      key.user_index = static_cast<uint8_t>(index);
      pad.keys.push_back(key);
    }
    pad.controls = controls;
    pad.packet++;
  }
  return true;
}

ScriptInputDriver::Pad* ScriptInputDriver::PadFor(DeviceId id, size_t* index) {
  const uint64_t raw = static_cast<uint64_t>(id);
  if (raw < kScriptPadBase || raw >= kScriptPadBase + pads_.size()) {
    return nullptr;
  }
  *index = static_cast<size_t>(raw - kScriptPadBase);
  return &pads_[*index];
}

void ScriptInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < pads_.size(); i++) {
    if (!Update(i)) {
      continue;
    }
    DeviceInfo info;
    info.id = static_cast<DeviceId>(kScriptPadBase + i);
    info.name = fmt::format("Script pad {}", i + 1);
    info.serial = fmt::format("script-pad-{}", i + 1);
    info.fixed_user = static_cast<int32_t>(i);
    info.transport = "script";
    out.push_back(info);
  }
}

X_RESULT ScriptInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t index = 0;
  Pad* pad = PadFor(id, &index);
  if (!pad || !Update(index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_state) {
    *out_state = {};
    out_state->packet_number = pad->packet;
    ControlsToGamepad(pad->controls, look_percent_, &out_state->gamepad);
    auto& g = out_state->gamepad;
    if (pad->live_mask & 1) g.thumb_lx = pad->live.thumb_lx;
    if (pad->live_mask & 2) g.thumb_ly = pad->live.thumb_ly;
    if (pad->live_mask & 4) g.thumb_rx = pad->live.thumb_rx;
    if (pad->live_mask & 8) g.thumb_ry = pad->live.thumb_ry;
    if (pad->live_mask & 16) g.left_trigger = pad->live.left_trigger;
    if (pad->live_mask & 32) g.right_trigger = pad->live.right_trigger;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t /*flags*/,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t index = 0;
  Pad* pad = PadFor(id, &index);
  if (!pad || !Update(index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    *out_caps = {};
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
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

X_RESULT ScriptInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* /*vibration*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t index = 0;
  Pad* pad = PadFor(id, &index);
  return pad && Update(index) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT ScriptInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t /*flags*/,
                                               X_INPUT_KEYSTROKE* out_keystroke) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t index = 0;
  Pad* pad = PadFor(id, &index);
  if (!pad || !Update(index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (pad->keys.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = pad->keys.front();
  }
  pad->keys.pop_front();
  return X_ERROR_SUCCESS;
}

std::string LivePadCommand(std::string_view command, std::string_view args) {
  if (command != "press" && command != "hold" && command != "release" && command != "stick" &&
      command != "trigger" && command != "pad") {
    return {};
  }
  std::vector<std::string> words;
  size_t slot = 0;
  for (std::string_view rest = Trim(args); !rest.empty(); rest = Trim(rest)) {
    const size_t sp = std::min(rest.find_first_of(" \t"), rest.size());
    std::string word = Lower(rest.substr(0, sp));
    rest.remove_prefix(sp);
    if (word.size() == 2 && word[0] == 'p' && word[1] >= '1' && word[1] <= '4') {
      slot = static_cast<size_t>(word[1] - '1');
    } else {
      words.push_back(std::move(word));
    }
  }
  const auto arg = [&](size_t i) { return i < words.size() ? std::string_view(words[i]) : std::string_view(); };
  uint32_t bits = 0;
  bool disconnect = false;
  const bool named = !arg(0).empty() && ControlBits(arg(0), bits, disconnect) && !disconnect;
  if (command == "press" && !named) {
    return {};  // not a pad control: the keyboard press of the test channel may know it
  }
  const size_t pads = g_live_pads.load();
  if (slot >= pads) {
    return fmt::format("error no virtual pad {} ({} in this game; start the live session with --pads N)",
                       slot + 1, pads);
  }
  const auto number = [](std::string_view text, double lo, double hi, double& out) {
    const std::string copy(text);
    char* end = nullptr;
    out = std::strtod(copy.c_str(), &end);
    return !copy.empty() && end == copy.c_str() + copy.size() && out >= lo && out <= hi;
  };
  std::lock_guard<std::mutex> lock(g_live_mutex);
  LivePad& p = g_live[slot];
  const double now = LiveNow();
  const std::string who = fmt::format("p{}", slot + 1);
  if (command == "press") {
    double ms = 150;
    if (!arg(1).empty() && !number(arg(1), 20, 10000, ms)) {
      return "error usage: press NAMES [20..10000 ms] [pN]";
    }
    const double start = std::max(now, p.queue_end + 0.06);  // taps queue, with a release gap between
    p.taps.push_back({start, start + ms / 1000.0, bits, false});
    p.queue_end = start + ms / 1000.0;
    return fmt::format("ok press {} {} in {} ms for {} ms", arg(0), who, static_cast<int>((start - now) * 1000),
                       static_cast<int>(ms));
  }
  if (command == "hold") {
    if (!named) return "error usage: hold NAMES [pN]";
    p.held |= bits;
    return fmt::format("ok hold {} {}", arg(0), who);
  }
  if (command == "release") {
    if (arg(0) == "all") {
      p = LivePad{};
      return "ok released all " + who;
    }
    if (!named) return "error usage: release NAMES|all [pN]";
    p.held &= ~bits;
    return fmt::format("ok release {} {}", arg(0), who);
  }
  if (command == "stick" || command == "trigger") {
    const bool stick = command == "stick";
    const int side = arg(0) == "l" ? 0 : arg(0) == "r" ? 1 : -1;
    double x = 0, y = 0, ms = 0;
    const size_t ms_at = stick ? 3 : 2;
    const bool off = arg(1) == "off";
    const bool ok = side >= 0 && (off || (stick ? number(arg(1), -1, 1, x) && number(arg(2), -1, 1, y)
                                                : number(arg(1), 0, 1, x))) &&
                    (off || arg(ms_at).empty() || number(arg(ms_at), 20, 60000, ms));
    if (!ok) {
      return stick ? "error usage: stick l|r X Y [ms] [pN] (X, Y -1..1, up = +Y) | stick l|r off"
                   : "error usage: trigger l|r V [ms] [pN] (V 0..1) | trigger l|r off";
    }
    const double until = off ? 0 : ms > 0 ? now + ms / 1000.0 : -1;
    if (stick) {
      p.axis[side * 2] = static_cast<int16_t>(std::lround(x * 32767));
      p.axis[side * 2 + 1] = static_cast<int16_t>(std::lround(y * 32767));
      p.axis_until[side * 2] = p.axis_until[side * 2 + 1] = until;
    } else {
      p.trig[side] = static_cast<uint8_t>(std::lround(x * 255));
      p.trig_until[side] = until;
    }
    return off ? fmt::format("ok {} {} off {}", command, arg(0), who)
               : fmt::format("ok {} {} {}{}{} {}", command, arg(0), x, stick ? fmt::format(" {}", y) : "",
                             ms > 0 ? fmt::format(" for {} ms", ms) : " held", who);
  }
  // pad: what the live layer applies now
  std::string out = fmt::format("ok {} of {} held 0x{:x} taps {}", who, pads, p.held, p.taps.size());
  const char* names[] = {"lx", "ly", "rx", "ry"};
  for (int i = 0; i < 4; i++) {
    if (LiveOn(p.axis_until[i], now)) out += fmt::format(" {} {}", names[i], p.axis[i]);
  }
  for (int i = 0; i < 2; i++) {
    if (LiveOn(p.trig_until[i], now)) out += fmt::format(" {} {}", i ? "rt" : "lt", p.trig[i]);
  }
  return out;
}

}  // namespace rex::input::script
