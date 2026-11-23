/**
 * @file        input/script/kbm_script.cpp
 * @brief       Scripted keyboard and mouse for tests. See kbm_script.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/script/kbm_script.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/script/script_events.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(kbm_test_script, "", "Input",
                      "Tests: a file of scripted keyboard and mouse steps (see kbm_script.h)");

namespace rex::input::script {

namespace {

struct Step {
  std::string op;
  std::vector<std::string> args;
};

// Splits a line into words; "double quotes" keep spaces.
std::vector<std::string> Words(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false, any = false;
  for (char c : line) {
    if (c == '"') {
      quoted = !quoted;
      any = true;
    } else if (!quoted && (c == ' ' || c == '\t')) {
      if (any) out.push_back(cur);
      cur.clear();
      any = false;
    } else {
      cur += c;
      any = true;
    }
  }
  if (any) out.push_back(cur);
  return out;
}

void SleepMs(int64_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

SDL_WindowID GameWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  const SDL_WindowID id = (windows && count > 0) ? SDL_GetWindowID(windows[0]) : 0;
  SDL_free(windows);
  return id;
}

class Runner {
 public:
  Runner(std::vector<Step> steps, KbmTargetLookup lookup)
      : steps_(std::move(steps)), lookup_(std::move(lookup)) {}

  void Run() {
    for (const auto& step : steps_) {
      if (!RunStep(step)) {
        REXLOG_WARN("[kbm] step '{}' failed; script stopped", step.op);
        return;
      }
    }
    REXLOG_INFO("[kbm] script done");
  }

 private:
  bool RunStep(const Step& s) {
    const auto& a = s.args;
    if (s.op == "wait_log" && !a.empty()) {
      const double timeout = a.size() > 1 ? std::stod(a[1]) : 300.0;
      const std::string name = LogEventName(a[0]);
      const auto start = std::chrono::steady_clock::now();
      while (ScriptEventCount(name) <= consumed_[name]) {
        if (std::chrono::steady_clock::now() - start > std::chrono::duration<double>(timeout)) {
          REXLOG_WARN("[kbm] timed out waiting for log text");
          return false;
        }
        SleepMs(50);
      }
      consumed_[name]++;
      return true;
    }
    if (s.op == "sleep" && !a.empty()) {
      SleepMs(static_cast<int64_t>(std::stod(a[0]) * 1000.0));
      return true;
    }
    if (s.op == "mark") {
      std::string label;
      for (const auto& w : a) label += (label.empty() ? "" : " ") + w;
      REXLOG_INFO("[kbm] mark {}", label);
      return true;
    }
    if (s.op == "key" && !a.empty()) {
      // Key names may contain spaces ("Left Shift+Tab"); a trailing number is the hold time in ms.
      std::vector<std::string> words = a;
      int hold = 60;
      if (words.size() > 1 && !words.back().empty() &&
          std::all_of(words.back().begin(), words.back().end(),
                      [](char c) { return c >= '0' && c <= '9'; })) {
        hold = std::atoi(words.back().c_str());
        words.pop_back();
      }
      std::string chord;
      for (const auto& w : words) chord += (chord.empty() ? "" : " ") + w;
      return Chord(chord, hold);
    }
    if (s.op == "type" && !a.empty()) {
      for (char c : a[0]) {
        if (!Chord(std::string(1, c), 50)) return false;
        SleepMs(80);
      }
      return true;
    }
    if ((s.op == "move" || s.op == "click" || s.op == "wheel" || s.op == "drag") && !a.empty()) {
      float r[4];
      if (!Target(a[0], r)) {
        REXLOG_WARN("[kbm] no target '{}'", a[0]);
        return false;
      }
      const float cx = (r[0] + r[2]) * 0.5f, cy = (r[1] + r[3]) * 0.5f;
      if (s.op == "move") {
        REXLOG_INFO("[kbm] move {}", a[0]);
        Move(cx, cy);
        SleepMs(150);
        return true;
      }
      if (s.op == "click") {
        const std::string which = a.size() > 1 ? a[1] : "left";
        const int button = which == "right" ? SDL_BUTTON_RIGHT : which == "thumb" ? SDL_BUTTON_X1 : SDL_BUTTON_LEFT;
        REXLOG_INFO("[kbm] click {} ({})", a[0], which);
        Move(cx, cy);
        SleepMs(120);
        Button(button, true, cx, cy);
        SleepMs(70);
        Button(button, false, cx, cy);
        SleepMs(250);
        return true;
      }
      if (s.op == "wheel" && a.size() > 1) {
        const float detents = std::stof(a[1]);
        REXLOG_INFO("[kbm] wheel {} {}", a[0], detents);
        Move(cx, cy);
        SleepMs(100);
        Wheel(detents, cx, cy);
        SleepMs(250);
        return true;
      }
      if (s.op == "drag" && a.size() > 2) {
        const float f0 = std::stof(a[1]), f1 = std::stof(a[2]);
        const float x0 = r[0] + (r[2] - r[0]) * f0, x1 = r[0] + (r[2] - r[0]) * f1;
        REXLOG_INFO("[kbm] drag {} {} -> {}", a[0], f0, f1);
        Move(x0, cy);
        SleepMs(120);
        Button(SDL_BUTTON_LEFT, true, x0, cy);
        for (int i = 1; i <= 10; ++i) {
          SleepMs(40);
          Move(x0 + (x1 - x0) * static_cast<float>(i) / 10.0f, cy);
        }
        SleepMs(80);
        Button(SDL_BUTTON_LEFT, false, x1, cy);
        SleepMs(250);
        return true;
      }
    }
    REXLOG_WARN("[kbm] unknown step '{}'", s.op);
    return false;
  }

  bool Target(const std::string& name, float r[4]) {
    if (!name.empty() && name[0] == '@') {
      float x = 0, y = 0;
      if (std::sscanf(name.c_str() + 1, "%f,%f", &x, &y) != 2) return false;
      r[0] = r[2] = x;
      r[1] = r[3] = y;
      return true;
    }
    // The Guide redraws its targets every frame; give a fresh one a moment.
    for (int i = 0; i < 40; ++i) {
      if (lookup_ && lookup_(name, r)) return true;
      SleepMs(50);
    }
    return false;
  }

  // Window fractions to SDL window coordinates.
  bool ToWindow(float fx, float fy, float& x, float& y) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* w = (windows && count > 0) ? windows[0] : nullptr;
    int ww = 0, wh = 0;
    if (w) SDL_GetWindowSize(w, &ww, &wh);
    SDL_free(windows);
    if (ww <= 0 || wh <= 0) return false;
    x = fx * static_cast<float>(ww);
    y = fy * static_cast<float>(wh);
    return true;
  }

  void Move(float fx, float fy) {
    float x, y;
    if (!ToWindow(fx, fy, x, y)) return;
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.timestamp = SDL_GetTicksNS();
    e.motion.windowID = GameWindow();
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = x - last_x_;
    e.motion.yrel = y - last_y_;
    last_x_ = x;
    last_y_ = y;
    SDL_PushEvent(&e);
  }

  void Button(Uint8 button, bool down, float fx, float fy) {
    float x, y;
    if (!ToWindow(fx, fy, x, y)) return;
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.timestamp = SDL_GetTicksNS();
    e.button.windowID = GameWindow();
    e.button.button = button;
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
  }

  void Wheel(float detents, float fx, float fy) {
    float x, y;
    if (!ToWindow(fx, fy, x, y)) return;
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.timestamp = SDL_GetTicksNS();
    e.wheel.windowID = GameWindow();
    e.wheel.y = detents;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    e.wheel.mouse_x = x;
    e.wheel.mouse_y = y;
    SDL_PushEvent(&e);
  }

  void Key(SDL_Scancode sc, bool down, SDL_Keymod mod) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = GameWindow();
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    e.key.mod = mod;
    e.key.down = down;
    SDL_PushEvent(&e);
  }

  // "Left Shift+Tab": hold the leading keys, tap the last.
  bool Chord(const std::string& chord, int hold_ms) {
    std::vector<SDL_Scancode> keys;
    size_t start = 0;
    while (start <= chord.size()) {
      size_t plus = chord.find('+', start);
      if (plus == start) plus = chord.find('+', start + 1);  // a lone "+"
      const std::string name = chord.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
      SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
      if (sc == SDL_SCANCODE_UNKNOWN && name == "Shift") sc = SDL_SCANCODE_LSHIFT;
      if (sc == SDL_SCANCODE_UNKNOWN) {
        REXLOG_WARN("[kbm] unknown key '{}'", name);
        return false;
      }
      keys.push_back(sc);
      if (plus == std::string::npos) break;
      start = plus + 1;
    }
    REXLOG_INFO("[kbm] key {}", chord);
    SDL_Keymod mod = SDL_KMOD_NONE;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
      if (keys[i] == SDL_SCANCODE_LSHIFT || keys[i] == SDL_SCANCODE_RSHIFT) mod |= SDL_KMOD_LSHIFT;
      Key(keys[i], true, mod);
      SleepMs(30);
    }
    Key(keys.back(), true, mod);
    SleepMs(hold_ms);
    Key(keys.back(), false, mod);
    for (size_t i = keys.size() - 1; i-- > 0;) {
      SleepMs(30);
      Key(keys[i], false, SDL_KMOD_NONE);
    }
    SleepMs(200);
    return true;
  }

  std::vector<Step> steps_;
  KbmTargetLookup lookup_;
  std::map<std::string, uint64_t> consumed_;
  float last_x_ = 0, last_y_ = 0;
};

}  // namespace

bool StartKbmScriptIfSet(KbmTargetLookup lookup) {
  const std::string path = REXCVAR_GET(kbm_test_script);
  if (path.empty()) return false;
  std::ifstream in(path);
  if (!in) {
    REXLOG_ERROR("kbm_test_script: cannot read '{}'", path);
    return false;
  }
  std::vector<Step> steps;
  std::string line;
  while (std::getline(in, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    auto words = Words(line);
    if (words.empty()) continue;
    Step s;
    s.op = words[0];
    s.args.assign(words.begin() + 1, words.end());
    if (s.op == "wait_log" && !s.args.empty()) WatchLogText(s.args[0]);
    steps.push_back(std::move(s));
  }
  REXLOG_INFO("[kbm] script {} with {} steps", path, steps.size());
  // The window runs minimized in tests: let the keyboard driver take its keys.
  rex::input::mnk::SetTestFocus(true);
  std::thread([steps = std::move(steps), lookup = std::move(lookup)]() mutable {
    IgnoreLogLinesHere ignore;
    Runner(std::move(steps), std::move(lookup)).Run();
  }).detach();
  return true;
}

}  // namespace rex::input::script
