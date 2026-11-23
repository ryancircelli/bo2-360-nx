/**
 * @file        ui/overlay/guide_overlay.cpp
 * @brief       The in-game Guide overlay. See rex/ui/overlay/guide_overlay.h.
 *
 * Looks: the size and place of the Xbox 360 Guide (a panel of about 60% x 65%
 * of the window over the dimmed game), styled like the 2005 "Blades"
 * dashboard: green blades with vertical labels on either side of the open
 * blade, a brushed-silver header, a white list with a green highlight bar, a
 * description pane, a title bar with the clock and an A/B/X/Y legend.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/guide_overlay.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/logging.h>

namespace rex::ui {

namespace {

namespace in = rex::input;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

ImFont* g_font_body = nullptr;
ImFont* g_font_head = nullptr;

// ---- Palette (Blades, 2005) -------------------------------------------------
constexpr ImU32 kGreenLight = IM_COL32(158, 222, 84, 255);
constexpr ImU32 kGreen = IM_COL32(96, 180, 40, 255);
constexpr ImU32 kGreenDark = IM_COL32(44, 112, 18, 255);
constexpr ImU32 kGreenDeep = IM_COL32(24, 70, 10, 255);
constexpr ImU32 kSilverTop = IM_COL32(240, 242, 244, 255);
constexpr ImU32 kSilverBottom = IM_COL32(176, 182, 188, 255);
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kListBg = IM_COL32(252, 252, 252, 255);
constexpr ImU32 kListAlt = IM_COL32(240, 243, 238, 255);
constexpr ImU32 kText = IM_COL32(38, 42, 46, 255);
constexpr ImU32 kTextGrey = IM_COL32(128, 134, 140, 255);
constexpr ImU32 kTextDisabled = IM_COL32(170, 174, 178, 255);
constexpr ImU32 kTitleTop = IM_COL32(44, 48, 52, 250);
constexpr ImU32 kTitleBottom = IM_COL32(14, 16, 18, 250);
constexpr ImU32 kButtonA = IM_COL32(96, 186, 40, 255);
constexpr ImU32 kButtonB = IM_COL32(214, 44, 36, 255);
constexpr ImU32 kButtonX = IM_COL32(38, 110, 214, 255);
constexpr ImU32 kButtonY = IM_COL32(242, 186, 20, 255);

ImU32 Alpha(ImU32 c, float a) {
  const uint32_t al = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * ((c >> IM_COL32_A_SHIFT) & 0xFF));
  return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

ImFont* Body() { return g_font_body ? g_font_body : ImGui::GetFont(); }
ImFont* Head() { return g_font_head ? g_font_head : Body(); }

ImVec2 TextSize(ImFont* font, float size, std::string_view text, float wrap = 0.0f) {
  return font->CalcTextSizeA(size, FLT_MAX, wrap, text.data(), text.data() + text.size());
}

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, std::string_view text,
          float wrap = 0.0f) {
  dl->AddText(font, size, pos, col, text.data(), text.data() + text.size(), wrap);
}

// Text read bottom to top, centred on `center` (the Blades labels).
void TextVertical(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                  std::string_view text) {
  const ImVec2 ts = TextSize(font, size, text);
  const int start = dl->VtxBuffer.Size;
  Text(dl, font, size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), col, text);
  for (int i = start; i < dl->VtxBuffer.Size; ++i) {
    ImVec2& p = dl->VtxBuffer[i].pos;
    const float dx = p.x - center.x, dy = p.y - center.y;
    p = ImVec2(center.x + dy, center.y - dx);
  }
}

// Brushed metal: a vertical gradient with fine horizontal streaks.
void Brushed(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom, float rounding = 0.0f,
             ImDrawFlags corners = ImDrawFlags_RoundCornersNone) {
  if (rounding > 0.0f) {
    dl->AddRectFilled(a, b, bottom, rounding, corners);
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + (corners & ImDrawFlags_RoundCornersTop ? rounding : 0)),
                                ImVec2(b.x, b.y - (corners & ImDrawFlags_RoundCornersBottom ? rounding : 0)),
                                top, top, bottom, bottom);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + rounding * 2.0f), top, rounding,
                      corners & ImDrawFlags_RoundCornersTop);
  } else {
    dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
  }
  uint32_t seed = 0x9E3779B9u ^ static_cast<uint32_t>(a.x * 7 + a.y * 13);
  for (float y = a.y + 1.0f; y < b.y - 1.0f; y += 2.0f) {
    seed = seed * 1664525u + 1013904223u;
    const float n = static_cast<float>((seed >> 24) & 0xFF) / 255.0f;
    const ImU32 c = n > 0.5f ? IM_COL32(255, 255, 255, static_cast<int>((n - 0.5f) * 70))
                             : IM_COL32(0, 0, 0, static_cast<int>((0.5f - n) * 30));
    dl->AddLine(ImVec2(a.x + rounding * 0.5f, y), ImVec2(b.x - rounding * 0.5f, y), c, 1.0f);
  }
}

// A round controller button glyph with its letter.
void ButtonGlyph(ImDrawList* dl, ImVec2 center, float r, ImU32 col, const char* letter) {
  dl->AddCircleFilled(ImVec2(center.x, center.y + r * 0.12f), r, IM_COL32(0, 0, 0, 90), 32);
  dl->AddCircleFilled(center, r, col, 32);
  dl->AddCircleFilled(ImVec2(center.x, center.y - r * 0.35f), r * 0.62f, IM_COL32(255, 255, 255, 60), 32);
  dl->AddCircle(center, r, IM_COL32(0, 0, 0, 120), 32, 1.0f);
  const float size = r * 1.35f;
  const ImVec2 ts = TextSize(Head(), size, letter);
  Text(dl, Head(), size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), kWhite, letter);
}

// The Xbox "orb": a green sphere with a white X.
void Orb(ImDrawList* dl, ImVec2 c, float r) {
  dl->AddCircleFilled(c, r, kGreenDark, 40);
  dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.08f), r * 0.9f, kGreen, 40);
  dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.38f), r * 0.55f, IM_COL32(255, 255, 255, 70), 40);
  const float k = r * 0.48f;
  dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), kWhite, r * 0.28f);
  dl->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x - k, c.y + k), kWhite, r * 0.28f);
}

std::string ClockText() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  int hour = tm.tm_hour % 12;
  if (hour == 0) hour = 12;
  std::snprintf(buf, sizeof(buf), "%d:%02d %s", hour, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  return buf;
}

bool Visible(const guide::Item& item) {
  return !item.visible || item.visible();
}

bool Enabled(const guide::Item& item) {
  return !item.enabled || item.enabled();
}

std::string ValueText(const guide::Item& item) {
  switch (item.type) {
    case guide::ItemType::kToggle:
      return item.get_bool && item.get_bool() ? "On" : "Off";
    case guide::ItemType::kChoice: {
      const int i = item.get_index ? item.get_index() : -1;
      if (i >= 0 && static_cast<size_t>(i) < item.choices.size()) return item.choices[i];
      return "-";
    }
    case guide::ItemType::kSlider: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), item.format.c_str(), item.get_value ? item.get_value() : 0.0);
      return buf;
    }
    case guide::ItemType::kAction:
      return item.text ? item.text() : std::string();
    case guide::ItemType::kInfo:
      return item.text ? item.text() : std::string();
  }
  return {};
}

bool Adjustable(const guide::Item& item) {
  return item.type == guide::ItemType::kToggle || item.type == guide::ItemType::kChoice ||
         item.type == guide::ItemType::kSlider;
}

}  // namespace

// ---- Fonts -----------------------------------------------------------------

void LoadGuideFonts(ImFontAtlas* atlas) {
  // Blades used a rounded sans; Arial Rounded / Arial are close and on every Mac.
  static const char* kBody[] = {
      "/System/Library/Fonts/Supplemental/Arial.ttf",
      "/Library/Fonts/Arial.ttf",
      "C:\\Windows\\Fonts\\segoeui.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  };
  static const char* kHead[] = {
      "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf",
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
      "C:\\Windows\\Fonts\\segoeuib.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
  };
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2190, 0x2193, 0x2022, 0x2022, 0};
  auto load = [&](const char* const* paths, size_t n, float size) -> ImFont* {
    for (size_t i = 0; i < n; ++i) {
      std::error_code ec;
      if (!std::filesystem::exists(paths[i], ec)) continue;
      ImFontConfig config;
      config.OversampleH = 2;
      config.OversampleV = 2;
      if (ImFont* font = atlas->AddFontFromFileTTF(paths[i], size, &config, kRanges)) {
        return font;
      }
    }
    return nullptr;
  };
  g_font_body = load(kBody, std::size(kBody), 30.0f);
  g_font_head = load(kHead, std::size(kHead), 40.0f);
  if (!g_font_body) {
    REXLOG_WARN("[guide] no TrueType font found; the Guide uses the default font");
  }
}

// ---- Input gate ------------------------------------------------------------

GuideInputGate::GuideInputGate(std::function<void(int user)> on_open) : on_open_(std::move(on_open)) {}

void GuideInputGate::Filter(uint32_t user, in::X_INPUT_STATE& state) {
  if (user >= was_down_.size()) return;
  uint16_t buttons = state.gamepad.buttons;
  const bool down = (buttons & in::X_INPUT_GAMEPAD_GUIDE) != 0;
  const bool rising = down && !was_down_[user];
  was_down_[user] = down;
  if (rising) {
    down_since_ms_[user] = NowMs();
    fired_[user] = false;
  }

  const int holder = holder_.load(std::memory_order_acquire);
  if (holder == static_cast<int>(user)) {
    // The Guide holds this player's pad: the game sees nothing.
    state.gamepad = {};
    return;
  }

  if (draining_.load(std::memory_order_acquire) == static_cast<int>(user)) {
    if (buttons != 0 || state.gamepad.left_trigger > 30 || state.gamepad.right_trigger > 30) {
      state.gamepad = {};
      return;
    }
    draining_.store(-1, std::memory_order_release);
  }

  if (in::GuideButtonToGame()) return;
  if (in::GuideButtonHold()) {
    if (down && !fired_[user] && NowMs() - down_since_ms_[user] >= REXCVAR_GET(guide_hold_ms)) {
      fired_[user] = true;
      if (holder < 0 && on_open_) on_open_(static_cast<int>(user));
    }
    if (fired_[user]) {
      state.gamepad.buttons = static_cast<uint16_t>(buttons & ~in::X_INPUT_GAMEPAD_GUIDE);
    }
    return;
  }
  // on
  state.gamepad.buttons = static_cast<uint16_t>(buttons & ~in::X_INPUT_GAMEPAD_GUIDE);
  if (rising && holder < 0 && on_open_) {
    on_open_(static_cast<int>(user));
  }
}

bool GuideInputGate::BlocksKeys(uint32_t user) const {
  return holder_.load(std::memory_order_acquire) == static_cast<int>(user);
}

in::InputSystem::GuestGate GuideInputGate::MakeGate() {
  in::InputSystem::GuestGate gate;
  gate.filter_state = [this](uint32_t user, in::X_INPUT_STATE& state) { Filter(user, state); };
  gate.block_keys = [this](uint32_t user) { return BlocksKeys(user); };
  return gate;
}

// ---- Dialog ----------------------------------------------------------------

GuideDialog::GuideDialog(ImGuiDrawer* imgui_drawer, int owner, Hooks hooks)
    : ImGuiDialog(imgui_drawer), owner_(owner), hooks_(std::move(hooks)) {
  Rebuild();
  // Open on the game's page when it has one, like the 360 Guide opens on Games.
  blade_ = 0;
  REXLOG_INFO("[guide] opened for player {}", owner_ + 1);
}

GuideDialog::~GuideDialog() {
  REXLOG_INFO("[guide] closed");
}

void GuideDialog::SelectBlade(std::string_view name) {
  pending_blade_ = std::string(name);
}

void GuideDialog::Rebuild() {
  pages_ = guide::Pages();
  std::vector<Blade> blades;
  for (size_t p = 0; p < pages_.size(); ++p) {
    const auto& page = pages_[p];
    auto it = std::find_if(blades.begin(), blades.end(),
                           [&](const Blade& b) { return b.name == page.blade; });
    if (it == blades.end()) {
      blades.push_back(Blade{page.blade, {}});
      it = blades.end() - 1;
    }
    // One entry per group, in the order the groups first appear.
    std::vector<std::string> groups;
    if (page.build) {
      groups.push_back({});
    } else {
      for (const auto& item : page.items) {
        if (!Visible(item)) continue;
        if (std::find(groups.begin(), groups.end(), item.group) == groups.end()) {
          groups.push_back(item.group);
        }
      }
    }
    for (const auto& g : groups) {
      Entry e;
      e.page = p;
      e.group = g;
      e.title = g.empty() ? page.title : g;
      e.description = g.empty() ? page.description : std::string();
      it->entries.push_back(std::move(e));
    }
  }
  const std::string current = blade_ < blades_.size() ? blades_[blade_].name : std::string();
  blades_ = std::move(blades);
  entry_sel_.resize(blades_.size(), 0);
  blade_ = 0;
  for (size_t i = 0; i < blades_.size(); ++i) {
    if (blades_[i].name == current) blade_ = i;
  }
  if (blades_.empty()) return;
  level_ = blades_[blade_].entries.size() == 1 ? 1 : level_;
}

std::vector<guide::Item> GuideDialog::ItemsFor(const Entry& entry) {
  std::vector<guide::Item> out;
  if (entry.page >= pages_.size()) return out;
  auto& page = pages_[entry.page];
  if (page.build) {
    for (auto& item : page.build()) {
      if (Visible(item)) out.push_back(std::move(item));
    }
    return out;
  }
  for (const auto& item : page.items) {
    if (item.group == entry.group && Visible(item)) out.push_back(item);
  }
  return out;
}

uint32_t GuideDialog::PollPads() {
  uint32_t bits = 0;
  if (!hooks_.snapshot) return bits;
  for (const auto& snap : hooks_.snapshot()) {
    // Keyboard-and-mouse pads would double the keyboard keys the Guide reads
    // itself; scripted and test pads (also synthetic) do drive it.
    if (!snap.has_state || snap.info.name == "Keyboard and Mouse" || snap.info.name == "None") continue;
    if (owner_ >= 0 && snap.user != owner_) continue;
    const auto& gp = snap.state.gamepad;
    const uint16_t b = gp.buttons;
    const int16_t lx = gp.thumb_lx, ly = gp.thumb_ly;
    constexpr int16_t kDead = 16000;
    if ((b & in::X_INPUT_GAMEPAD_DPAD_UP) || ly > kDead) bits |= kUp;
    if ((b & in::X_INPUT_GAMEPAD_DPAD_DOWN) || ly < -kDead) bits |= kDown;
    if ((b & in::X_INPUT_GAMEPAD_DPAD_LEFT) || lx < -kDead) bits |= kLeft;
    if ((b & in::X_INPUT_GAMEPAD_DPAD_RIGHT) || lx > kDead) bits |= kRight;
    if (b & in::X_INPUT_GAMEPAD_A) bits |= kA;
    if (b & in::X_INPUT_GAMEPAD_B) bits |= kB;
    if (b & in::X_INPUT_GAMEPAD_X) bits |= kX;
    if (b & in::X_INPUT_GAMEPAD_Y) bits |= kY;
    if (b & in::X_INPUT_GAMEPAD_LEFT_SHOULDER) bits |= kLB;
    if (b & in::X_INPUT_GAMEPAD_RIGHT_SHOULDER) bits |= kRB;
    if (b & in::X_INPUT_GAMEPAD_GUIDE) bits |= kGuide;
  }
  return bits;
}

uint32_t GuideDialog::PollKeyboard() {
  uint32_t pressed = 0;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  if (key(ImGuiKey_UpArrow) || key(ImGuiKey_W)) pressed |= kUp;
  if (key(ImGuiKey_DownArrow) || key(ImGuiKey_S)) pressed |= kDown;
  if (key(ImGuiKey_LeftArrow) || key(ImGuiKey_A)) pressed |= kLeft;
  if (key(ImGuiKey_RightArrow) || key(ImGuiKey_D)) pressed |= kRight;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    pressed |= kA;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
    pressed |= kB;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Q, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) pressed |= kLB;
  if (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) pressed |= kRB;
  if (ImGui::IsKeyPressed(ImGuiKey_R, false)) pressed |= kX;
  return pressed;
}

void GuideDialog::RequestClose() {
  if (close_requested_) return;
  close_requested_ = true;
  if (hooks_.request_close) hooks_.request_close();
}

void GuideDialog::SwitchBlade(int delta) {
  if (blades_.empty()) return;
  const int n = static_cast<int>(blades_.size());
  blade_ = static_cast<size_t>(((static_cast<int>(blade_) + delta) % n + n) % n);
  level_ = blades_[blade_].entries.size() == 1 ? 1 : 0;
  item_sel_ = 0;
  scroll_ = 0.0f;
  REXLOG_INFO("[guide] blade {}", blades_[blade_].name);
}

void GuideDialog::Back() {
  if (level_ == 1 && !blades_.empty() && blades_[blade_].entries.size() > 1) {
    level_ = 0;
    scroll_ = 0.0f;
    REXLOG_INFO("[guide] back to {}", blades_[blade_].name);
  } else {
    RequestClose();
  }
}

void GuideDialog::Activate(guide::Item& item, int direction) {
  if (!Enabled(item)) return;
  switch (item.type) {
    case guide::ItemType::kToggle: {
      if (!item.get_bool || !item.set_bool) return;
      const bool now = item.get_bool();
      const bool next = direction == 0 ? !now : direction > 0;
      if (next != now) {
        item.set_bool(next);
        if (item.cvar.empty()) guide::LogChange(item.label, next ? "on" : "off");
      }
      break;
    }
    case guide::ItemType::kChoice: {
      if (!item.get_index || !item.set_index || item.choices.empty()) return;
      const int n = static_cast<int>(item.choices.size());
      int i = item.get_index();
      if (i < 0) i = 0;
      const int next = ((i + (direction < 0 ? -1 : 1)) % n + n) % n;
      item.set_index(next);
      if (item.cvar.empty()) guide::LogChange(item.label, item.choices[next]);
      break;
    }
    case guide::ItemType::kSlider: {
      if (!item.get_value || !item.set_value) return;
      const double v = item.get_value();
      double next = v + (direction < 0 ? -item.step : item.step);
      if (direction == 0 && v >= item.max) next = item.min;  // A wraps
      next = std::clamp(next, item.min, item.max);
      if (next != v) {
        item.set_value(next);
        if (item.cvar.empty()) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), item.format.c_str(), next);
          guide::LogChange(item.label, buf);
        }
      }
      break;
    }
    case guide::ItemType::kAction:
      if (direction == 0 && item.action) {
        guide::LogChange(item.label, "run");
        item.action();
      }
      break;
    case guide::ItemType::kInfo:
      break;
  }
}

void GuideDialog::HandleButtons(uint32_t pressed, std::vector<guide::Item>& items) {
  if (pressed & kGuide) {
    RequestClose();
    return;
  }
  if (pressed & kY) {
    RequestClose();
    return;
  }
  if (pressed & kLB) SwitchBlade(-1);
  if (pressed & kRB) SwitchBlade(+1);
  if (pressed & (kLB | kRB)) return;
  if (pressed & kB) {
    Back();
    return;
  }
  if (blades_.empty()) return;
  Blade& blade = blades_[blade_];
  if (level_ == 0) {
    int& sel = entry_sel_[blade_];
    const int n = static_cast<int>(blade.entries.size());
    if (pressed & kUp) sel = std::max(0, sel - 1);
    if (pressed & kDown) sel = std::min(n - 1, sel + 1);
    if (pressed & kLeft) SwitchBlade(-1);
    else if (pressed & kRight) SwitchBlade(+1);
    else if ((pressed & kA) && n > 0) {
      level_ = 1;
      item_sel_ = 0;
      scroll_ = 0.0f;
      REXLOG_INFO("[guide] page {} / {}", blade.name, blade.entries[static_cast<size_t>(sel)].title);
    }
    return;
  }
  const int n = static_cast<int>(items.size());
  if (n == 0) {
    if (pressed & kLeft) SwitchBlade(-1);
    if (pressed & kRight) SwitchBlade(+1);
    return;
  }
  item_sel_ = std::clamp(item_sel_, 0, n - 1);
  if (pressed & kUp) item_sel_ = (item_sel_ + n - 1) % n;
  if (pressed & kDown) item_sel_ = (item_sel_ + 1) % n;
  guide::Item& item = items[static_cast<size_t>(item_sel_)];
  if (pressed & (kLeft | kRight)) {
    if (Adjustable(item)) {
      Activate(item, (pressed & kLeft) ? -1 : +1);
    } else {
      SwitchBlade((pressed & kLeft) ? -1 : +1);
    }
    return;
  }
  if (pressed & kA) Activate(item, 0);
  if ((pressed & kX) && !item.cvar.empty() && Enabled(item)) {
    rex::cvar::ResetToDefault(item.cvar);
    guide::LogChange(item.label, "default (" + rex::cvar::GetFlagByName(item.cvar) + ")");
  }
}

void GuideDialog::OnDraw(ImGuiIO& io) {
  if (close_requested_) return;
  if (!pending_blade_.empty()) {
    for (size_t i = 0; i < blades_.size(); ++i) {
      if (blades_[i].name == pending_blade_) {
        blade_ = i;
        level_ = blades_[i].entries.size() == 1 ? 1 : 0;
      }
    }
    pending_blade_.clear();
  }

  // Pages may be registered while the Guide is open (a game registering late).
  {
    const auto now_pages = guide::Pages();
    if (now_pages.size() != pages_.size()) Rebuild();
  }

  // Pad edges, with auto-repeat for held directions.
  const uint32_t buttons = PollPads();
  if (first_poll_) {
    // The press that opened the Guide (and anything held) is not an action.
    prev_buttons_ = buttons;
    first_poll_ = false;
  }
  uint32_t pressed = buttons & ~prev_buttons_;
  const int64_t now = NowMs();
  constexpr uint32_t kDirs = kUp | kDown | kLeft | kRight;
  if (pressed & kDirs) {
    repeat_button_ = pressed & kDirs;
    repeat_at_ms_ = now + 420;
  } else if (repeat_button_ && (buttons & repeat_button_) == repeat_button_) {
    if (now >= repeat_at_ms_) {
      pressed |= repeat_button_;
      repeat_at_ms_ = now + 110;
    }
  } else {
    repeat_button_ = 0;
  }
  prev_buttons_ = buttons;
  pressed |= PollKeyboard();

  std::vector<guide::Item> items;
  if (!blades_.empty() && level_ == 1) {
    const auto& blade = blades_[blade_];
    const int sel = std::clamp(entry_sel_[blade_], 0, std::max(0, static_cast<int>(blade.entries.size()) - 1));
    if (!blade.entries.empty()) items = ItemsFor(blade.entries[static_cast<size_t>(sel)]);
  }

  // Mouse from the last frame's hit rects.
  const ImVec2 mouse = io.MousePos;
  const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const bool right_click = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
  bool clicked_something = false;
  for (const auto& h : hits_) {
    if (mouse.x < h.x0 || mouse.x >= h.x1 || mouse.y < h.y0 || mouse.y >= h.y1) continue;
    if (h.kind == 0 && click) {
      SwitchBlade(h.index - static_cast<int>(blade_));
      clicked_something = true;
    } else if (h.kind == 1) {
      if (moved || click) {
        if (level_ == 0) entry_sel_[blade_] = h.index;
        else item_sel_ = h.index;
      }
      if (click) {
        pressed |= kA;
        clicked_something = true;
      }
    } else if (h.kind == 2 && click) {
      pressed |= h.index;
      clicked_something = true;
    } else if (h.kind == 3) {
      clicked_something = clicked_something || click;
    }
  }
  if (right_click) pressed |= kB;
  if (io.MouseWheel != 0.0f) pressed |= io.MouseWheel > 0 ? kUp : kDown;
  const bool clicked_outside = click && !clicked_something && !hits_.empty();
  if (clicked_outside) {
    // Outside the panel: like B at the top.
    RequestClose();
    return;
  }

  if (pressed) {
    const size_t blade_before = blade_;
    const int level_before = level_;
    HandleButtons(pressed, items);
    if (close_requested_) return;
    if (blade_before != blade_ || level_before != level_) {
      items.clear();
      if (!blades_.empty() && level_ == 1) {
        const auto& blade = blades_[blade_];
        const int sel = std::clamp(entry_sel_[blade_], 0, std::max(0, static_cast<int>(blade.entries.size()) - 1));
        if (!blade.entries.empty()) items = ItemsFor(blade.entries[static_cast<size_t>(sel)]);
      }
    }
  }

  // ---- Draw ----------------------------------------------------------------
  const ImVec2 display = io.DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(display);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##xbox_guide", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Dim the game.
    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 140));
    // The 360 Guide's footprint: about 60% x 65%, centred, never too small.
    float w = std::max(display.x * 0.60f, std::min(display.x - 16.0f, 620.0f));
    float h = std::max(display.y * 0.65f, std::min(display.y - 16.0f, 400.0f));
    w = std::min(w, h * 1.75f);
    const float x = std::floor((display.x - w) * 0.5f);
    const float y = std::floor((display.y - h) * 0.5f);
    hits_.clear();
    DrawPanel(dl, x, y, w, h, items);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void GuideDialog::DrawPanel(ImDrawList* dl, float x, float y, float w, float h,
                            std::vector<guide::Item>& items) {
  const float s = h / 700.0f;  // design units: a 700 px tall panel
  const float r = 14.0f * s;
  ImFont* body = Body();
  ImFont* head = Head();
  const float fs_body = std::max(11.0f, 21.0f * s);
  const float fs_small = std::max(10.0f, 17.0f * s);
  const float fs_head = std::max(14.0f, 34.0f * s);
  const float fs_blade = std::max(11.0f, 22.0f * s);

  hits_.push_back({x, y, x + w, y + h, 3, 0});

  // Shadow + frame.
  for (int i = 4; i >= 1; --i) {
    const float o = static_cast<float>(i) * 4.0f * s;
    dl->AddRectFilled(ImVec2(x - o, y - o + 6 * s), ImVec2(x + w + o, y + h + o + 6 * s),
                      IM_COL32(0, 0, 0, 22), r + o);
  }
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kGreenDeep, r);

  // Title bar.
  const float th = 64.0f * s;
  dl->AddRectFilledMultiColor(ImVec2(x, y + r), ImVec2(x + w, y + th), kTitleTop, kTitleTop,
                              kTitleBottom, kTitleBottom);
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + r * 2.0f), kTitleTop, r, ImDrawFlags_RoundCornersTop);
  dl->AddLine(ImVec2(x + r, y + th - 1), ImVec2(x + w - r, y + th - 1), Alpha(kGreenLight, 0.8f), 2.0f * s);
  const float orb_r = 20.0f * s;
  Orb(dl, ImVec2(x + 22.0f * s + orb_r, y + th * 0.5f), orb_r);
  {
    const char* title = "Xbox Guide";
    const ImVec2 ts = TextSize(head, fs_head, title);
    Text(dl, head, fs_head, ImVec2(x + 34.0f * s + orb_r * 2.0f, y + (th - ts.y) * 0.5f), kWhite, title);
    const std::string clock = ClockText();
    const ImVec2 cs = TextSize(body, fs_body * 1.1f, clock);
    Text(dl, body, fs_body * 1.1f, ImVec2(x + w - cs.x - 24.0f * s, y + (th - cs.y) * 0.5f),
         IM_COL32(220, 224, 228, 255), clock);
  }

  // Legend bar.
  const float lh = 58.0f * s;
  const float ly = y + h - lh;
  dl->AddRectFilledMultiColor(ImVec2(x, ly), ImVec2(x + w, y + h - r), kTitleBottom, kTitleBottom,
                              kTitleTop, kTitleTop);
  dl->AddRectFilled(ImVec2(x, y + h - r * 2.0f), ImVec2(x + w, y + h), kTitleTop, r,
                    ImDrawFlags_RoundCornersBottom);
  dl->AddLine(ImVec2(x + r, ly), ImVec2(x + w - r, ly), Alpha(kGreenLight, 0.6f), 1.5f * s);

  // Body: blades either side of the open one.
  const float by0 = y + th;
  const float by1 = ly;
  const float strip = std::clamp(48.0f * s, 24.0f, 64.0f);
  const size_t nblades = blades_.size();
  const float left_w = strip * static_cast<float>(blade_);
  const float right_w = strip * static_cast<float>(nblades > 0 ? nblades - 1 - blade_ : 0);
  for (size_t i = 0; i < nblades; ++i) {
    if (i == blade_) continue;
    float sx;
    if (i < blade_) sx = x + strip * static_cast<float>(i);
    else sx = x + w - right_w + strip * static_cast<float>(i - blade_ - 1);
    const ImVec2 a(sx, by0), b(sx + strip, by1);
    // Green blade: lighter at its leading edge, with a bright rim.
    const ImU32 lead = i < blade_ ? kGreenLight : kGreen;
    const ImU32 trail = i < blade_ ? kGreen : kGreenLight;
    dl->AddRectFilledMultiColor(a, b, lead, trail, Alpha(trail, 1.0f), lead);
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + (b.y - a.y) * 0.35f), IM_COL32(255, 255, 255, 60),
                                IM_COL32(255, 255, 255, 60), IM_COL32(255, 255, 255, 0),
                                IM_COL32(255, 255, 255, 0));
    dl->AddLine(ImVec2(i < blade_ ? b.x - 1 : a.x, a.y), ImVec2(i < blade_ ? b.x - 1 : a.x, b.y),
                kGreenDeep, 2.0f * s);
    dl->AddLine(ImVec2(i < blade_ ? a.x + 1 : b.x - 1, a.y), ImVec2(i < blade_ ? a.x + 1 : b.x - 1, b.y),
                IM_COL32(255, 255, 255, 90), 1.0f);
    std::string label = blades_[i].name;
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    TextVertical(dl, head, fs_blade, ImVec2(a.x + strip * 0.5f, b.y - 16.0f * s - TextSize(head, fs_blade, label).x * 0.5f),
                 kWhite, label);
    hits_.push_back({a.x, a.y, b.x, b.y, 0, static_cast<int>(i)});
  }

  // The open blade.
  const float ax0 = x + left_w, ax1 = x + w - right_w;
  if (nblades == 0) return;
  const Blade& blade = blades_[blade_];
  // Its own edge: a green spine with the vertical label, like the Blades front blade.
  const float spine = strip * 0.9f;
  dl->AddRectFilledMultiColor(ImVec2(ax0, by0), ImVec2(ax0 + spine, by1), kGreenDark, kGreen, kGreen, kGreenDark);
  {
    std::string label = blade.name;
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    TextVertical(dl, head, fs_blade, ImVec2(ax0 + spine * 0.5f, by1 - 16.0f * s - TextSize(head, fs_blade, label).x * 0.5f),
                 kWhite, label);
  }
  const float cx0 = ax0 + spine;
  const float cx1 = ax1;
  // Brushed-silver header with the blade (and entry) name.
  const float hh = 64.0f * s;
  Brushed(dl, ImVec2(cx0, by0), ImVec2(cx1, by0 + hh), kSilverTop, kSilverBottom);
  dl->AddLine(ImVec2(cx0, by0 + hh), ImVec2(cx1, by0 + hh), kGreenDark, 2.0f * s);
  {
    std::string heading = blade.name;
    if (level_ == 1 && blade.entries.size() > 1) {
      const int sel = std::clamp(entry_sel_[blade_], 0, static_cast<int>(blade.entries.size()) - 1);
      heading += "  \xC2\xBB  " + blade.entries[static_cast<size_t>(sel)].title;
    }
    const ImVec2 ts = TextSize(head, fs_head, heading);
    Text(dl, head, fs_head, ImVec2(cx0 + 20.0f * s + 1, by0 + (hh - ts.y) * 0.5f + 1),
         IM_COL32(255, 255, 255, 180), heading);
    Text(dl, head, fs_head, ImVec2(cx0 + 20.0f * s, by0 + (hh - ts.y) * 0.5f), kGreenDark, heading);
  }

  // List (white) and description pane (silver).
  const float pad = 14.0f * s;
  const float list_x0 = cx0 + pad;
  const float list_y0 = by0 + hh + pad;
  const float list_y1 = by1 - pad;
  const float list_x1 = cx0 + (cx1 - cx0) * 0.60f;
  const float desc_x0 = list_x1 + pad;
  const float desc_x1 = cx1 - pad;
  // Content background: pale silver.
  Brushed(dl, ImVec2(cx0, by0 + hh + 2 * s), ImVec2(cx1, by1), IM_COL32(226, 230, 232, 255),
          IM_COL32(196, 202, 206, 255));
  dl->AddRectFilled(ImVec2(list_x0 + 2 * s, list_y0 + 3 * s), ImVec2(list_x1 + 2 * s, list_y1 + 3 * s),
                    IM_COL32(0, 0, 0, 40), 8.0f * s);
  dl->AddRectFilled(ImVec2(list_x0, list_y0), ImVec2(list_x1, list_y1), kListBg, 8.0f * s);
  dl->AddRect(ImVec2(list_x0, list_y0), ImVec2(list_x1, list_y1), IM_COL32(150, 158, 164, 255), 8.0f * s, 0, 1.0f);

  const float row_h = 44.0f * s;
  struct Row {
    std::string label, value;
    bool enabled = true;
    bool header = false;
    const guide::Item* item = nullptr;
  };
  std::vector<Row> rows;
  int sel = 0;
  if (level_ == 0) {
    for (const auto& e : blade.entries) rows.push_back({e.title, "\xE2\x80\xBA", true, false, nullptr});
    sel = std::clamp(entry_sel_[blade_], 0, std::max(0, static_cast<int>(rows.size()) - 1));
  } else {
    for (const auto& item : items) {
      rows.push_back({item.label, ValueText(item), Enabled(item), false, &item});
    }
    sel = rows.empty() ? 0 : std::clamp(item_sel_, 0, static_cast<int>(rows.size()) - 1);
  }
  const float view_h = list_y1 - list_y0 - 8.0f * s;
  const float content_h = row_h * static_cast<float>(rows.size());
  // Keep the highlight in view.
  const float sel_top = row_h * static_cast<float>(sel);
  if (sel_top < scroll_) scroll_ = sel_top;
  if (sel_top + row_h > scroll_ + view_h) scroll_ = sel_top + row_h - view_h;
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content_h - view_h));

  dl->PushClipRect(ImVec2(list_x0 + 1, list_y0 + 4 * s), ImVec2(list_x1 - 1, list_y1 - 4 * s), true);
  for (size_t i = 0; i < rows.size(); ++i) {
    const float ry0 = list_y0 + 4.0f * s + row_h * static_cast<float>(i) - scroll_;
    const float ry1 = ry0 + row_h;
    if (ry1 < list_y0 || ry0 > list_y1) continue;
    const bool selected = static_cast<int>(i) == sel;
    const Row& row = rows[i];
    if (!selected && (i % 2) == 1) {
      dl->AddRectFilled(ImVec2(list_x0 + 4 * s, ry0), ImVec2(list_x1 - 4 * s, ry1), kListAlt, 4.0f * s);
    }
    if (selected) {
      const ImVec2 a(list_x0 + 4 * s, ry0 + 1 * s), b(list_x1 - 4 * s, ry1 - 1 * s);
      dl->AddRectFilled(a, b, kGreen, 6.0f * s);
      dl->AddRectFilledMultiColor(ImVec2(a.x + 2 * s, a.y + 2 * s), ImVec2(b.x - 2 * s, a.y + (b.y - a.y) * 0.5f),
                                  IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 90),
                                  IM_COL32(255, 255, 255, 10), IM_COL32(255, 255, 255, 10));
      dl->AddRect(a, b, kGreenDark, 6.0f * s, 0, 1.5f * s);
    }
    const ImU32 col = selected ? kWhite : (row.enabled ? kText : kTextDisabled);
    const ImVec2 ls = TextSize(body, fs_body, row.label);
    Text(dl, body, fs_body, ImVec2(list_x0 + 18.0f * s, ry0 + (row_h - ls.y) * 0.5f), col, row.label);
    std::string value = row.value;
    if (row.item && row.item->type == guide::ItemType::kChoice && selected) value = "\xE2\x80\xB9 " + value + " \xE2\x80\xBA";
    if (row.item && row.item->type == guide::ItemType::kAction && value.empty()) value = "\xE2\x80\xBA";
    const ImU32 vcol = selected ? kWhite : (row.item && row.item->type == guide::ItemType::kInfo ? kTextGrey : kGreenDark);
    if (row.item && row.item->type == guide::ItemType::kToggle) {
      // A Blades-style check box.
      const bool on = value == "On";
      const float bs = 20.0f * s;
      const ImVec2 bc(list_x1 - 30.0f * s, ry0 + row_h * 0.5f);
      dl->AddRectFilled(ImVec2(bc.x - bs * 0.5f, bc.y - bs * 0.5f), ImVec2(bc.x + bs * 0.5f, bc.y + bs * 0.5f),
                        selected ? IM_COL32(255, 255, 255, 230) : IM_COL32(236, 240, 236, 255), 4.0f * s);
      dl->AddRect(ImVec2(bc.x - bs * 0.5f, bc.y - bs * 0.5f), ImVec2(bc.x + bs * 0.5f, bc.y + bs * 0.5f),
                  kGreenDark, 4.0f * s, 0, 1.5f * s);
      if (on) {
        dl->AddLine(ImVec2(bc.x - bs * 0.3f, bc.y), ImVec2(bc.x - bs * 0.05f, bc.y + bs * 0.28f), kGreen, 3.0f * s);
        dl->AddLine(ImVec2(bc.x - bs * 0.05f, bc.y + bs * 0.28f), ImVec2(bc.x + bs * 0.35f, bc.y - bs * 0.3f), kGreen,
                    3.0f * s);
      }
      const ImVec2 vs = TextSize(body, fs_small, value);
      Text(dl, body, fs_small, ImVec2(bc.x - bs * 0.5f - 10.0f * s - vs.x, ry0 + (row_h - vs.y) * 0.5f), vcol, value);
    } else if (row.item && row.item->type == guide::ItemType::kSlider && row.item->max > row.item->min) {
      const float bw = 110.0f * s;
      const float bx1 = list_x1 - 22.0f * s;
      const float bx0 = bx1 - bw;
      const float bcy = ry0 + row_h * 0.5f;
      const double v = row.item->get_value ? row.item->get_value() : row.item->min;
      const float t = static_cast<float>((v - row.item->min) / (row.item->max - row.item->min));
      dl->AddRectFilled(ImVec2(bx0, bcy - 4 * s), ImVec2(bx1, bcy + 4 * s), IM_COL32(200, 206, 200, 255), 4 * s);
      dl->AddRectFilled(ImVec2(bx0, bcy - 4 * s), ImVec2(bx0 + bw * std::clamp(t, 0.0f, 1.0f), bcy + 4 * s),
                        selected ? kWhite : kGreen, 4 * s);
      const ImVec2 vs = TextSize(body, fs_small, value);
      Text(dl, body, fs_small, ImVec2(bx0 - 10.0f * s - vs.x, ry0 + (row_h - vs.y) * 0.5f), vcol, value);
    } else if (!value.empty()) {
      const ImVec2 vs = TextSize(body, fs_small, value);
      Text(dl, body, fs_small, ImVec2(list_x1 - 20.0f * s - vs.x, ry0 + (row_h - vs.y) * 0.5f), vcol, value);
    }
    hits_.push_back({list_x0, std::max(ry0, list_y0), list_x1, std::min(ry1, list_y1), 1, static_cast<int>(i)});
  }
  dl->PopClipRect();
  if (rows.empty()) {
    const char* empty = "Nothing here yet";
    const ImVec2 es = TextSize(body, fs_body, empty);
    Text(dl, body, fs_body, ImVec2((list_x0 + list_x1 - es.x) * 0.5f, list_y0 + 30 * s), kTextGrey, empty);
  }
  // Scroll bar.
  if (content_h > view_h) {
    const float track0 = list_y0 + 8 * s, track1 = list_y1 - 8 * s;
    const float t0 = track0 + (track1 - track0) * (scroll_ / content_h);
    const float t1 = track0 + (track1 - track0) * std::min(1.0f, (scroll_ + view_h) / content_h);
    dl->AddRectFilled(ImVec2(list_x1 - 7 * s, t0), ImVec2(list_x1 - 3 * s, t1), Alpha(kGreenDark, 0.7f), 2 * s);
  }

  // Description pane.
  {
    const ImVec2 a(desc_x0, list_y0), b(desc_x1, list_y1);
    dl->AddRectFilled(a, b, IM_COL32(246, 248, 246, 235), 8.0f * s);
    dl->AddRect(a, b, IM_COL32(150, 158, 164, 255), 8.0f * s, 0, 1.0f);
    dl->AddRectFilledMultiColor(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + 6 * s), kGreen, kGreenLight,
                                kGreenLight, kGreen);
    std::string title, text, value, footnote;
    if (level_ == 0 && !blade.entries.empty()) {
      const auto& e = blade.entries[static_cast<size_t>(sel)];
      title = e.title;
      text = e.description.empty() ? "Press A to open." : e.description;
    } else if (level_ == 1 && !items.empty()) {
      const auto& item = items[static_cast<size_t>(sel)];
      title = item.label;
      text = item.description;
      value = ValueText(item);
      if (!item.cvar.empty()) footnote = "Setting: " + item.cvar;
      if (!Enabled(item)) footnote = footnote.empty() ? "Not available right now" : footnote + " (not available now)";
    }
    float ty = a.y + 18.0f * s;
    const float wrap = (b.x - a.x) - 32.0f * s;
    if (!title.empty()) {
      const ImVec2 ts = TextSize(head, fs_body * 1.15f, title, wrap);
      Text(dl, head, fs_body * 1.15f, ImVec2(a.x + 16 * s, ty), kGreenDark, title, wrap);
      ty += ts.y + 10.0f * s;
    }
    if (!value.empty()) {
      const std::string line = "Now: " + value;
      const ImVec2 vs = TextSize(body, fs_small, line, wrap);
      Text(dl, body, fs_small, ImVec2(a.x + 16 * s, ty), kText, line, wrap);
      ty += vs.y + 8.0f * s;
    }
    if (!text.empty()) {
      Text(dl, body, fs_small, ImVec2(a.x + 16 * s, ty), IM_COL32(70, 76, 82, 255), text, wrap);
    }
    if (!footnote.empty()) {
      const ImVec2 fs = TextSize(body, fs_small * 0.85f, footnote, wrap);
      Text(dl, body, fs_small * 0.85f, ImVec2(a.x + 16 * s, b.y - fs.y - 12 * s), kTextGrey, footnote, wrap);
    }
  }

  // Legend: A Select, B Back, X Default, Y Close.
  {
    struct Legend {
      ImU32 col;
      const char* letter;
      const char* text;
      uint32_t button;
    };
    const bool can_default = level_ == 1 && !items.empty() &&
                             !items[static_cast<size_t>(std::clamp(item_sel_, 0, static_cast<int>(items.size()) - 1))].cvar.empty();
    std::vector<Legend> legend = {{kButtonA, "A", "Select", kA}, {kButtonB, "B", "Back", kB}};
    if (can_default) legend.push_back({kButtonX, "X", "Default", kX});
    legend.push_back({kButtonY, "Y", "Close Guide", kY});
    const float gr = 15.0f * s;
    float lx = x + 28.0f * s;
    const float cy = ly + lh * 0.5f;
    for (const auto& l : legend) {
      ButtonGlyph(dl, ImVec2(lx + gr, cy), gr, l.col, l.letter);
      const ImVec2 ts = TextSize(body, fs_body, l.text);
      Text(dl, body, fs_body, ImVec2(lx + gr * 2.0f + 8.0f * s, cy - ts.y * 0.5f), IM_COL32(230, 234, 236, 255), l.text);
      hits_.push_back({lx, ly, lx + gr * 2.0f + 8.0f * s + ts.x, ly + lh, 2, static_cast<int>(l.button)});
      lx += gr * 2.0f + 8.0f * s + ts.x + 30.0f * s;
    }
    const char* hint = "LB / RB: blades";
    const ImVec2 hs = TextSize(body, fs_small, hint);
    Text(dl, body, fs_small, ImVec2(x + w - hs.x - 24.0f * s, cy - hs.y * 0.5f), IM_COL32(160, 168, 172, 255), hint);
  }
  // Outer rim.
  dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), Alpha(kGreenLight, 0.9f), r, 0, 2.0f * s);
}

}  // namespace rex::ui
