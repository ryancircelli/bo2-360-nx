/**
 * @file        ui/overlay/guide_overlay.cpp
 * @brief       The in-game Guide overlay. See rex/ui/overlay/guide_overlay.h.
 *
 * Looks: the size and place of the Xbox 360 Guide (about 60% x 65% of the
 * window over the dimmed game), drawn as the 2005 "Blades" dashboard. Every
 * blade is a tall bowed fin with rounded ends: the closed ones stand at the
 * sides, silver tinted with their section colour, each with a glossy tab
 * carrying its name read top to bottom. The open blade is its section colour,
 * with a title band, a legend band and a grey-white brushed content panel
 * between them. The bow comes from one warp of the whole drawing (Warp):
 * shapes are built straight, tessellated top to bottom, and every vertex is
 * pushed out from the centre line by an amount that peaks at mid height, so
 * the left edges draw as "(" and the right ones as ")". Switching blades
 * slides them across. All of it is ImGui draw-list geometry, no images.
 *
 * Input: a controller, the keyboard or the mouse each drive all of it. While
 * the Guide is open the game gets nothing from the pad that opened it, nor
 * from the keyboard and mouse (rex/input/mnk hold).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/guide_overlay.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

namespace rex::ui {

namespace {

namespace in = rex::input;

constexpr float kPi = 3.14159265f;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

ImFont* g_font_body = nullptr;
ImFont* g_font_head = nullptr;

// ---- Colours ----------------------------------------------------------------

struct Rgb {
  float r, g, b;
};

Rgb Mix(Rgb a, Rgb b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
Rgb Scale(Rgb a, float k) {
  return {a.r * k, a.g * k, a.b * k};
}
ImU32 Col(Rgb c, float alpha = 1.0f) {
  auto ch = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 255.0f)); };
  return IM_COL32(ch(c.r), ch(c.g), ch(c.b), ch(alpha * 255.0f));
}

constexpr Rgb kWhiteRgb{255, 255, 255};
constexpr Rgb kSilver{214, 218, 222};
constexpr Rgb kSilverDark{132, 138, 146};
constexpr Rgb kPanelTop{244, 245, 246};
constexpr Rgb kPanelBottom{212, 215, 219};
constexpr Rgb kOutline{70, 76, 84};
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kText = IM_COL32(46, 50, 56, 255);
constexpr ImU32 kTextGrey = IM_COL32(112, 118, 126, 255);
constexpr ImU32 kTextDisabled = IM_COL32(160, 164, 170, 255);
constexpr ImU32 kButtonA = IM_COL32(96, 186, 40, 255);
constexpr ImU32 kButtonB = IM_COL32(214, 44, 36, 255);
constexpr ImU32 kButtonX = IM_COL32(38, 110, 214, 255);
constexpr ImU32 kButtonY = IM_COL32(242, 186, 20, 255);

// Each blade's own colour, as the Blades dashboard gave each section one.
Rgb SectionColour(std::string_view blade, size_t index) {
  if (blade == "Game") return {92, 178, 34};
  if (blade == "System") return {70, 132, 196};
  if (blade == "Controllers") return {236, 140, 26};
  if (blade == "Leave Game") return {200, 56, 46};
  static const Rgb kOthers[] = {{150, 84, 190}, {28, 160, 160}, {204, 170, 30}};
  return kOthers[index % std::size(kOthers)];
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

// Text with a soft drop shadow.
void TextShadow(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, std::string_view text,
                float off, ImU32 shadow = IM_COL32(0, 0, 0, 110)) {
  Text(dl, font, size, ImVec2(pos.x + off, pos.y + off), shadow, text);
  Text(dl, font, size, pos, col, text);
}

// Text read top to bottom (the Blades labels), its first letter at `top`.
void TextDown(ImDrawList* dl, ImFont* font, float size, ImVec2 top, ImU32 col, std::string_view text) {
  const ImVec2 ts = TextSize(font, size, text);
  const ImVec2 center(top.x, top.y + ts.x * 0.5f);
  const int start = dl->VtxBuffer.Size;
  Text(dl, font, size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), col, text);
  for (int i = start; i < dl->VtxBuffer.Size; ++i) {
    ImVec2& p = dl->VtxBuffer[i].pos;
    const float dx = p.x - center.x, dy = p.y - center.y;
    p = ImVec2(center.x - dy, center.y + dx);
  }
}

// How far a rounded rectangle's side sits in at height d from its top or
// bottom (radius r).
float CornerInset(float d, float r) {
  if (r <= 0.0f || d >= r) return 0.0f;
  const float e = r - std::max(d, 0.0f);
  return r - std::sqrt(std::max(0.0f, r * r - e * e));
}

uint32_t Hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// A rounded rectangle tessellated into rows (dense, so the warp can bend its
// sides) and `cols` columns, each vertex coloured by colour(u, v, row): u and
// v run 0..1 across and down.
template <typename F>
void Mesh(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, int cols, float row_px, F colour) {
  const float h = b.y - a.y, w = b.x - a.x;
  if (h <= 1.0f || w <= 1.0f) return;
  r = std::min(r, std::min(w, h) * 0.5f);
  std::vector<float> ys;
  // Fine rows through the rounded ends, coarser in between.
  const int end_rows = r > 0.0f ? std::max(4, static_cast<int>(r / 2.0f)) : 0;
  for (int i = 0; i <= end_rows; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(std::max(1, end_rows));
    ys.push_back(a.y + r * (1.0f - std::cos(t * kPi * 0.5f)));
  }
  const float mid0 = a.y + r, mid1 = b.y - r;
  const int mid_rows = std::max(1, static_cast<int>((mid1 - mid0) / row_px));
  for (int i = 1; i < mid_rows; ++i) ys.push_back(mid0 + (mid1 - mid0) * static_cast<float>(i) / mid_rows);
  for (int i = end_rows; i >= 0; --i) {
    const float t = static_cast<float>(i) / static_cast<float>(std::max(1, end_rows));
    ys.push_back(b.y - r * (1.0f - std::cos(t * kPi * 0.5f)));
  }
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end(), [](float p, float q) { return std::fabs(p - q) < 0.05f; }),
           ys.end());
  const int rows = static_cast<int>(ys.size());
  if (rows < 2) return;
  const int vtx = rows * (cols + 1);
  const int idx = (rows - 1) * cols * 6;
  dl->PrimReserve(idx, vtx);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  for (int ri = 0; ri < rows; ++ri) {
    const float y = ys[ri];
    const float inset = CornerInset(std::min(y - a.y, b.y - y), r);
    const float x0 = a.x + inset, x1 = b.x - inset;
    const float v = (y - a.y) / h;
    for (int c = 0; c <= cols; ++c) {
      const float t = static_cast<float>(c) / static_cast<float>(cols);
      const float x = x0 + (x1 - x0) * t;
      dl->PrimWriteVtx(ImVec2(x, y), uv, colour((x - a.x) / w, v, ri));
    }
  }
  for (int ri = 0; ri + 1 < rows; ++ri) {
    for (int c = 0; c < cols; ++c) {
      const ImDrawIdx i0 = static_cast<ImDrawIdx>(base + ri * (cols + 1) + c);
      const ImDrawIdx i1 = static_cast<ImDrawIdx>(i0 + 1);
      const ImDrawIdx i2 = static_cast<ImDrawIdx>(i0 + (cols + 1));
      const ImDrawIdx i3 = static_cast<ImDrawIdx>(i2 + 1);
      dl->PrimWriteIdx(i0);
      dl->PrimWriteIdx(i1);
      dl->PrimWriteIdx(i3);
      dl->PrimWriteIdx(i0);
      dl->PrimWriteIdx(i3);
      dl->PrimWriteIdx(i2);
    }
  }
}

// The outline of a rounded rectangle, with its sides cut into short pieces so
// the warp bends them. Drawn anti-aliased over a Mesh to smooth its edges.
void Outline(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 col, float thickness) {
  r = std::min(r, std::min(b.x - a.x, b.y - a.y) * 0.5f);
  dl->PathClear();
  const int side = 24;
  auto arc = [&](ImVec2 c, float a0) {
    for (int i = 0; i <= 8; ++i) {
      const float t = a0 + (kPi * 0.5f) * static_cast<float>(i) / 8.0f;
      dl->PathLineTo(ImVec2(c.x + std::cos(t) * r, c.y + std::sin(t) * r));
    }
  };
  arc(ImVec2(a.x + r, a.y + r), kPi);                // top left
  arc(ImVec2(b.x - r, a.y + r), kPi * 1.5f);         // top right
  for (int i = 1; i < side; ++i) dl->PathLineTo(ImVec2(b.x, a.y + r + (b.y - a.y - 2 * r) * i / side));
  arc(ImVec2(b.x - r, b.y - r), 0.0f);               // bottom right
  arc(ImVec2(a.x + r, b.y - r), kPi * 0.5f);         // bottom left
  for (int i = 1; i < side; ++i) dl->PathLineTo(ImVec2(a.x, b.y - r - (b.y - a.y - 2 * r) * i / side));
  dl->PathStroke(col, ImDrawFlags_Closed, thickness);
}

// A round controller button glyph with its letter.
void ButtonGlyph(ImDrawList* dl, ImVec2 center, float r, ImU32 col, const char* letter) {
  dl->AddCircleFilled(ImVec2(center.x, center.y + r * 0.12f), r, IM_COL32(0, 0, 0, 90), 28);
  dl->AddCircleFilled(center, r, col, 28);
  dl->AddCircleFilled(ImVec2(center.x, center.y - r * 0.35f), r * 0.62f, IM_COL32(255, 255, 255, 70), 28);
  dl->AddCircle(center, r, IM_COL32(0, 0, 0, 120), 28, 1.0f);
  const float size = r * 1.35f;
  const ImVec2 ts = TextSize(Head(), size, letter);
  Text(dl, Head(), size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), kWhite, letter);
}

// A glossy pill (selection bars, buttons): base colour, a light top half.
void GlossPill(ImDrawList* dl, ImVec2 a, ImVec2 b, Rgb base, float r, float alpha = 1.0f) {
  const Rgb top = Mix(base, kWhiteRgb, 0.45f), bottom = Scale(base, 0.78f);
  Mesh(dl, a, b, r, 1, 4.0f, [&](float, float v, int) {
    return Col(v < 0.5f ? Mix(top, base, v * 2.0f) : Mix(base, bottom, (v - 0.5f) * 2.0f), alpha);
  });
  dl->AddRect(a, b, Col(Scale(base, 0.55f), alpha), r, 0, 1.0f);
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

std::string FormatValue(const guide::Item& item, double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), item.format.empty() ? "%g" : item.format.c_str(), v);
  return buf;
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
    case guide::ItemType::kSlider:
      return FormatValue(item, item.get_value ? item.get_value() : 0.0);
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

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// The named targets the scripted tests click (GuideTarget).
std::mutex g_targets_mutex;
std::map<std::string, std::array<float, 4>, std::less<>> g_targets;

}  // namespace

bool GuideTarget(std::string_view name, float rect[4]) {
  std::lock_guard lock(g_targets_mutex);
  auto it = g_targets.find(name);
  if (it == g_targets.end()) return false;
  for (int i = 0; i < 4; ++i) rect[i] = it->second[i];
  return true;
}

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
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2190, 0x2193, 0x2022, 0x2022, 0x2039, 0x203A, 0};
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

void GuideInputGate::SetHolder(int user) {
  const int prev = holder_.exchange(user, std::memory_order_acq_rel);
  if (user >= 0 && prev < 0) {
    held_polls_.store(0, std::memory_order_relaxed);
    held_input_polls_.store(0, std::memory_order_relaxed);
    in::mnk::SetOverlayHold(true);
  }
  if (user < 0 && prev >= 0) {
    draining_.store(prev, std::memory_order_release);
    in::mnk::SetOverlayHold(false);
    REXLOG_INFO("[guide] player {} pad held while open: {} game polls, {} with input held back, 0 passed",
                prev + 1, held_polls_.load(), held_input_polls_.load());
  }
}

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
    const auto& g = state.gamepad;
    held_polls_.fetch_add(1, std::memory_order_relaxed);
    if (g.buttons || g.left_trigger > 30 || g.right_trigger > 30 || std::abs(g.thumb_lx) > 8000 ||
        std::abs(g.thumb_ly) > 8000 || std::abs(g.thumb_rx) > 8000 || std::abs(g.thumb_ry) > 8000) {
      held_input_polls_.fetch_add(1, std::memory_order_relaxed);
    }
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
  anim_from_ = 0;
  REXLOG_INFO("[guide] opened for player {}", owner_ + 1);
}

GuideDialog::~GuideDialog() {
  {
    std::lock_guard lock(g_targets_mutex);
    g_targets.clear();
  }
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
  auto once = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
  if (key(ImGuiKey_UpArrow) || key(ImGuiKey_W)) pressed |= kUp;
  if (key(ImGuiKey_DownArrow) || key(ImGuiKey_S)) pressed |= kDown;
  if (key(ImGuiKey_LeftArrow) || key(ImGuiKey_A)) pressed |= kLeft;
  if (key(ImGuiKey_RightArrow) || key(ImGuiKey_D)) pressed |= kRight;
  if (once(ImGuiKey_Enter) || once(ImGuiKey_KeypadEnter) || once(ImGuiKey_Space)) pressed |= kA;
  if (once(ImGuiKey_Escape) || once(ImGuiKey_Backspace)) pressed |= kB;
  const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
  if (once(ImGuiKey_Q) || once(ImGuiKey_PageUp) || (once(ImGuiKey_Tab) && shift)) pressed |= kLB;
  if (once(ImGuiKey_E) || once(ImGuiKey_PageDown) || (once(ImGuiKey_Tab) && !shift)) pressed |= kRB;
  if (once(ImGuiKey_R)) pressed |= kX;
  return pressed;
}

std::vector<guide::Item> GuideDialog::CurrentItems() {
  std::vector<guide::Item> items;
  if (blades_.empty() || level_ != 1) return items;
  const auto& blade = blades_[blade_];
  if (blade.entries.empty()) return items;
  const int sel = std::clamp(entry_sel_[blade_], 0, static_cast<int>(blade.entries.size()) - 1);
  return ItemsFor(blade.entries[static_cast<size_t>(sel)]);
}

void GuideDialog::SetSlider(guide::Item& item, double value) {
  if (!Enabled(item) || !item.get_value || !item.set_value) return;
  double next = std::clamp(value, item.min, item.max);
  if (item.step > 0.0) {
    next = item.min + std::round((next - item.min) / item.step) * item.step;
    next = std::clamp(next, item.min, item.max);
  }
  if (next == item.get_value()) return;
  item.set_value(next);
  if (item.cvar.empty()) guide::LogChange(item.label, FormatValue(item, next));
}

// Typing a number on a slider row: digits, '.', '-' start an edit box; Enter
// sets it, Escape drops it, Backspace deletes. Returns true while the edit
// owns the keyboard.
bool GuideDialog::HandleNumberEntry(std::vector<guide::Item>& items) {
  if (level_ != 1 || items.empty()) {
    editing_ = false;
    return false;
  }
  const int row = std::clamp(item_sel_, 0, static_cast<int>(items.size()) - 1);
  guide::Item& item = items[static_cast<size_t>(row)];
  if (editing_ && edit_row_ != row) editing_ = false;
  if (item.type != guide::ItemType::kSlider || !Enabled(item)) {
    editing_ = false;
    return false;
  }
  std::string typed;
  for (int k = 0; k <= 9; ++k) {
    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + k), false) ||
        ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + k), false)) {
      typed += static_cast<char>('0' + k);
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Period, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false)) {
    typed += '.';
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) {
    typed += '-';
  }
  if (!editing_) {
    if (typed.empty()) return false;
    editing_ = true;
    edit_row_ = row;
    edit_text_.clear();
  }
  if (edit_text_.size() < 12) edit_text_ += typed;
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !edit_text_.empty()) edit_text_.pop_back();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    editing_ = false;
    return true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    editing_ = false;
    char* end = nullptr;
    const double v = std::strtod(edit_text_.c_str(), &end);
    if (end && end != edit_text_.c_str()) {
      REXLOG_INFO("[guide] typed {} for {}", edit_text_, item.label);
      SetSlider(item, v);
    }
    return true;
  }
  return true;
}

void GuideDialog::RequestClose() {
  if (close_requested_) return;
  close_requested_ = true;
  if (hooks_.request_close) hooks_.request_close();
}

void GuideDialog::SwitchBlade(int delta) {
  if (blades_.empty() || delta == 0) return;
  const int n = static_cast<int>(blades_.size());
  anim_from_ = blade_;
  anim_start_ms_ = NowMs();
  blade_ = static_cast<size_t>(((static_cast<int>(blade_) + delta) % n + n) % n);
  level_ = blades_[blade_].entries.size() == 1 ? 1 : 0;
  item_sel_ = 0;
  scroll_ = 0.0f;
  editing_ = false;
  drag_row_ = -1;
  ensure_visible_ = true;
  REXLOG_INFO("[guide] blade {}", blades_[blade_].name);
}

void GuideDialog::Back() {
  editing_ = false;
  if (level_ == 1 && !blades_.empty() && blades_[blade_].entries.size() > 1) {
    level_ = 0;
    scroll_ = 0.0f;
    ensure_visible_ = true;
    REXLOG_INFO("[guide] back to {}", blades_[blade_].name);
  } else {
    RequestClose();
  }
}

void GuideDialog::HandleButtons(uint32_t pressed, std::vector<guide::Item>& items) {
  if (pressed & (kGuide | kY)) {
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
  if (pressed & (kUp | kDown)) ensure_visible_ = true;
  Blade& blade = blades_[blade_];
  if (level_ == 0) {
    int& sel = entry_sel_[blade_];
    const int n = static_cast<int>(blade.entries.size());
    if (pressed & kUp) sel = std::max(0, sel - 1);
    if (pressed & kDown) sel = std::min(n - 1, sel + 1);
    if (pressed & kLeft) {
      SwitchBlade(-1);
    } else if (pressed & kRight) {
      SwitchBlade(+1);
    } else if ((pressed & kA) && n > 0) {
      level_ = 1;
      item_sel_ = 0;
      scroll_ = 0.0f;
      ensure_visible_ = true;
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
        anim_from_ = i;
        level_ = blades_[i].entries.size() == 1 ? 1 : 0;
      }
    }
    pending_blade_.clear();
  }

  // Pages may be registered while the Guide is open (a game registering late).
  if (guide::Pages().size() != pages_.size()) Rebuild();

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

  std::vector<guide::Item> items = CurrentItems();
  if (!HandleNumberEntry(items)) pressed |= PollKeyboard();

  // Mouse, against the last frame's targets (topmost first). Targets are
  // kept unwarped, so unwarp the pointer.
  const ImVec2 mouse_raw = io.MousePos;
  const bool mouse_valid = ImGui::IsMousePosValid(&mouse_raw);
  const ImVec2 mouse(mouse_valid ? UnwarpX(mouse_raw.x, mouse_raw.y) : -1e9f, mouse_raw.y);
  const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const bool right_click = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
  const HitRect* hit = nullptr;
  const HitRect* hit_row = nullptr;
  for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
    if (mouse.x < it->x0 || mouse.x >= it->x1 || mouse.y < it->y0 || mouse.y >= it->y1) continue;
    if (!hit) hit = &*it;
    if (it->kind == kHitRow && !hit_row) hit_row = &*it;
    if (hit && hit_row) break;
  }
  hover_row_ = hit_row ? hit_row->index : -1;
  hover_back_ = hit && hit->kind == kHitBack;
  hover_close_ = hit && hit->kind == kHitClose;
  if (hit_row && (moved || click)) {
    // Hover moves the highlight, like the pad does.
    if (level_ == 0) {
      entry_sel_[blade_] = hit_row->index;
    } else if (item_sel_ != hit_row->index) {
      item_sel_ = hit_row->index;
      editing_ = false;
    }
  }
  bool handled_click = false;
  if (click && hit) {
    handled_click = true;
    switch (hit->kind) {
      case kHitBlade:
        if (static_cast<size_t>(hit->index) != blade_) SwitchBlade(hit->index - static_cast<int>(blade_));
        break;
      case kHitRow:
        if (level_ == 0) {
          pressed |= kA;
        } else if (hit->index < static_cast<int>(items.size())) {
          guide::Item& item = items[static_cast<size_t>(hit->index)];
          if (item.type != guide::ItemType::kSlider) pressed |= kA;
        }
        break;
      case kHitSlider:
        if (level_ == 1 && hit->index < static_cast<int>(items.size())) {
          guide::Item& item = items[static_cast<size_t>(hit->index)];
          if (Enabled(item) && item.get_value) {
            item_sel_ = hit->index;
            drag_row_ = hit->index;
            drag_x0_ = hit->x0;
            drag_x1_ = hit->x1;
            drag_value_ = item.get_value();
          }
        }
        break;
      case kHitPrev:
      case kHitNext:
        if (level_ == 1 && hit->index < static_cast<int>(items.size())) {
          item_sel_ = hit->index;
          Activate(items[static_cast<size_t>(hit->index)], hit->kind == kHitPrev ? -1 : +1);
        }
        break;
      case kHitLegend:
        pressed |= static_cast<uint32_t>(hit->index);
        break;
      case kHitBack:
        pressed |= kB;
        break;
      case kHitClose:
        RequestClose();
        return;
      default:
        break;
    }
  }
  // Slider drag: follow the pointer, set the value on release.
  if (drag_row_ >= 0) {
    if (drag_row_ < static_cast<int>(items.size()) && drag_x1_ > drag_x0_) {
      guide::Item& item = items[static_cast<size_t>(drag_row_)];
      const float t = std::clamp((mouse.x - drag_x0_) / (drag_x1_ - drag_x0_), 0.0f, 1.0f);
      drag_value_ = item.min + (item.max - item.min) * t;
      if (item.step > 0.0) drag_value_ = item.min + std::round((drag_value_ - item.min) / item.step) * item.step;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        SetSlider(item, drag_value_);
        drag_row_ = -1;
      }
    } else {
      drag_row_ = -1;
    }
  }
  if (right_click) pressed |= kB;
  if (io.MouseWheel != 0.0f && row_h_ > 0.0f) {
    if (overflow_) {
      // The wheel scrolls the list; the highlight follows the pointer.
      scroll_ -= io.MouseWheel * row_h_;
      ensure_visible_ = false;
    } else {
      pressed |= io.MouseWheel > 0 ? kUp : kDown;
    }
  }
  if (click && !handled_click && !hits_.empty()) {
    // Outside the Guide: close it.
    RequestClose();
    return;
  }

  if (pressed) {
    const size_t blade_before = blade_;
    const int level_before = level_;
    HandleButtons(pressed, items);
    if (close_requested_) return;
    if (blade_before != blade_ || level_before != level_) items = CurrentItems();
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
    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 150));
    // The 360 Guide's footprint: about 60% x 65%, centred, never too small.
    float w = std::max(display.x * 0.60f, std::min(display.x - 16.0f, 620.0f));
    float h = std::max(display.y * 0.65f, std::min(display.y - 16.0f, 400.0f));
    w = std::min(w, h * 1.75f);
    const float x = std::floor((display.x - w) * 0.5f);
    const float y = std::floor((display.y - h) * 0.5f);
    hits_.clear();
    DrawGuide(dl, x, y, w, h, items);
    PublishTargets(display.x, display.y);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
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

// ---- Drawing -----------------------------------------------------------------

namespace {

float Bulge(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return std::sin(t * kPi);
}

float EaseOut(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

constexpr int64_t kSlideMs = 260;

}  // namespace

ImVec2 GuideDialog::Warp(float x, float y) const {
  const float b = Bulge((y - warp_y0_) / warp_h_);
  return ImVec2(x + warp_k_ * b * (x - warp_cx_), y);
}

float GuideDialog::UnwarpX(float x, float y) const {
  const float b = Bulge((y - warp_y0_) / warp_h_);
  return warp_cx_ + (x - warp_cx_) / (1.0f + warp_k_ * b);
}

void GuideDialog::WarpVertices(ImDrawList* dl, int first_vertex) const {
  for (int i = first_vertex; i < dl->VtxBuffer.Size; ++i) {
    ImVec2& p = dl->VtxBuffer[i].pos;
    p = Warp(p.x, p.y);
  }
}

// Blade strips: the closed blades before the open one stand at the left, the
// ones after it at the right; each shows a tab `tab` wide.
std::vector<GuideDialog::Span> GuideDialog::Layout(size_t open, float x, float w) const {
  const size_t n = blades_.size();
  std::vector<Span> spans(n);
  const float tab = 54.0f * s_;
  const float under = 40.0f * s_;  // how far a closed blade runs under its neighbour
  const float open_l = x + static_cast<float>(open) * tab;
  // The open blade's span includes its own tab.
  const float open_r = x + w - static_cast<float>(n - 1 - open) * tab;
  for (size_t i = 0; i < n; ++i) {
    if (i < open) {
      const float l = x + static_cast<float>(i) * tab;
      spans[i] = {l, l + tab + under};
    } else if (i == open) {
      spans[i] = {open_l, open_r};
    } else {
      const float r = open_r + static_cast<float>(i - open) * tab;
      spans[i] = {r - tab - under, r};
    }
  }
  return spans;
}

void GuideDialog::DrawGuide(ImDrawList* dl, float x, float y, float w, float h,
                            std::vector<guide::Item>& items) {
  const float s = h / 700.0f;
  s_ = s;
  // The bow: the Guide's outer edges sit `bow` further out at mid height than
  // at the ends. Build everything inside the box less that bow.
  const float bow = 58.0f * s;
  const float nx = x + bow, nw = w - 2.0f * bow;
  warp_cx_ = x + w * 0.5f;
  warp_y0_ = y;
  warp_h_ = h;
  warp_k_ = bow / (nw * 0.5f);
  const int first_vertex = dl->VtxBuffer.Size;

  // Hits: the whole Guide box swallows clicks (outside it closes).
  hits_.push_back({nx, y, nx + nw, y + h, kHitPanel, 0, ""});

  const size_t n = blades_.size();
  if (n == 0) {
    DrawOpenBlade(dl, Span{nx, nx + nw}, y, h, items);
    WarpVertices(dl, first_vertex);
    return;
  }
  const std::vector<Span> to = Layout(blade_, nx, nw);
  std::vector<Span> cur = to;
  const int64_t elapsed = NowMs() - anim_start_ms_;
  const bool sliding = anim_from_ != blade_ && elapsed < kSlideMs;
  if (sliding) {
    const std::vector<Span> from = Layout(anim_from_, nx, nw);
    const float t = EaseOut(static_cast<float>(elapsed) / static_cast<float>(kSlideMs));
    for (size_t i = 0; i < n; ++i) {
      cur[i].l = from[i].l + (to[i].l - from[i].l) * t;
      cur[i].r = from[i].r + (to[i].r - from[i].r) * t;
    }
  } else {
    anim_from_ = blade_;
  }
  const float closed_top = y + 18.0f * s, closed_h = h - 36.0f * s;
  // Left stack, outermost first; then the right stack, outermost first.
  for (size_t i = 0; i < n; ++i) {
    if (i == blade_) break;
    if (i == anim_from_ && sliding) continue;
    DrawClosedBlade(dl, i, cur[i], true, closed_top, closed_h);
  }
  for (size_t i = n; i-- > blade_ + 1;) {
    if (i == anim_from_ && sliding) continue;
    DrawClosedBlade(dl, i, cur[i], false, closed_top, closed_h);
  }
  // The blade that was open slides shut under the one opening.
  if (sliding) DrawClosedBlade(dl, anim_from_, cur[anim_from_], anim_from_ < blade_, closed_top, closed_h);
  // The open blade keeps its own tab at its left edge, in its full colour, and
  // the body opens out of it.
  const float tab = 54.0f * s, under = 40.0f * s;
  const Span body{cur[blade_].l + tab, cur[blade_].r};
  DrawClosedBlade(dl, blade_, Span{cur[blade_].l, cur[blade_].l + tab + under}, true, closed_top, closed_h, true);
  DrawOpenBlade(dl, body, y, h, items);
  WarpVertices(dl, first_vertex);
}

void GuideDialog::DrawClosedBlade(ImDrawList* dl, size_t i, Span span, bool left, float y, float h, bool own) {
  const float s = s_;
  const Rgb section = SectionColour(blades_[i].name, i);
  const ImVec2 a(span.l, y), b(span.r, y + h);
  const float r = 30.0f * s;
  // Shadow on the blade beneath.
  const float sh = (left ? 7.0f : -7.0f) * s;
  Mesh(dl, ImVec2(a.x + sh, a.y + 4 * s), ImVec2(b.x + sh, b.y + 4 * s), r, 1, 12.0f,
       [](float, float, int) { return IM_COL32(0, 0, 0, 70); });
  // Silver fin, lit from the upper left, a faint wash of its section colour.
  const Rgb base = own ? Mix(section, kWhiteRgb, 0.30f) : Mix(kSilver, section, 0.10f);
  Mesh(dl, a, b, r, 12, 6.0f, [&](float u, float v, int) {
    const float gu = left ? u : 1.0f - u;
    const float shine = std::exp(-std::pow((gu - 0.22f) / 0.16f, 2.0f));
    Rgb c = Mix(base, kWhiteRgb, 0.65f * shine);
    c = Mix(c, kSilverDark, 0.45f * std::pow(1.0f - gu, 6.0f) + 0.30f * v * v);
    return Col(c);
  });
  Outline(dl, a, b, r, Col(kOutline, 0.85f), 1.3f);
  // The visible strip's glossy tab with the name, read top to bottom.
  const float tab = 54.0f * s;
  const float tx0 = left ? span.l + 7.0f * s : span.r - tab + 7.0f * s;
  const float tx1 = tx0 + tab - 14.0f * s;
  const float fs = std::max(11.0f, 25.0f * s);
  const float label_len = TextSize(Body(), fs, blades_[i].name).x;
  const ImVec2 ta(tx0, y + 12.0f * s), tb(tx1, y + std::max(h * 0.40f, label_len + 52.0f * s));
  const Rgb top = Mix(section, kWhiteRgb, 0.50f), dark = Scale(section, 0.72f);
  Mesh(dl, ta, tb, (tx1 - tx0) * 0.5f, 8, 4.0f, [&](float u, float v, int) {
    Rgb c = Mix(top, section, std::min(1.0f, v * 2.2f));
    c = Mix(c, dark, std::max(0.0f, v - 0.55f) * 1.6f);
    const float gloss = std::exp(-std::pow((u - 0.32f) / 0.20f, 2.0f)) * (1.0f - v);
    c = Mix(c, kWhiteRgb, 0.55f * gloss);
    return Col(c);
  });
  Outline(dl, ta, tb, (tx1 - tx0) * 0.5f, Col(Scale(section, 0.5f)), 1.2f);
  const float cx = (tx0 + tx1) * 0.5f;
  TextDown(dl, Body(), fs, ImVec2(cx + 1.2f * s, ta.y + 22.0f * s + 1.2f * s), IM_COL32(0, 0, 0, 120),
           blades_[i].name);
  TextDown(dl, Body(), fs, ImVec2(cx, ta.y + 22.0f * s), kWhite, blades_[i].name);
  // The whole visible strip is a click target.
  const float hx0 = left ? span.l : span.r - tab, hx1 = hx0 + tab;
  hits_.push_back({hx0, y, hx1, y + h, kHitBlade, static_cast<int>(i), "blade:" + blades_[i].name});
}

void GuideDialog::DrawOpenBlade(ImDrawList* dl, Span span, float y, float h, std::vector<guide::Item>& items) {
  const float s = s_;
  const std::string name = blades_.empty() ? std::string("Guide") : blades_[blade_].name;
  const Rgb section = blades_.empty() ? SectionColour("Game", 0) : SectionColour(name, blade_);
  const Rgb light = Mix(section, kWhiteRgb, 0.42f), deep = Scale(section, 0.55f);
  const ImVec2 a(span.l, y), b(span.r, y + h);
  const float r = 40.0f * s;
  const float title_h = 96.0f * s, legend_h = 70.0f * s;
  const float wv = b.x - a.x;
  // Shadows on both sides.
  Mesh(dl, ImVec2(a.x - 9 * s, a.y + 6 * s), ImVec2(b.x + 9 * s, b.y + 8 * s), r, 1, 12.0f,
       [](float, float, int) { return IM_COL32(0, 0, 0, 80); });
  // The blade: the section colour with a lighter sweeping swirl; a darker
  // title band whose lower edge dips at the centre, and a legend band whose
  // upper edge rises at the centre.
  Mesh(dl, a, b, r, 72, 3.0f, [&](float u, float v, int) {
    const float py = v * h;
    const float arc = std::sin(u * kPi);
    const float band_top = title_h + 14.0f * s * arc;
    const float band_bottom = h - legend_h - 10.0f * s * arc;
    Rgb c;
    if (py < band_top) {
      const float t = py / band_top;
      c = Mix(Mix(section, kWhiteRgb, 0.25f), deep, t);
      c = Mix(c, kWhiteRgb, 0.35f * std::max(0.0f, 0.5f - t));  // top gloss
    } else if (py > band_bottom) {
      const float t = (py - band_bottom) / (h - band_bottom);
      c = Mix(deep, Scale(section, 0.40f), t);
      c = Mix(c, light, 0.25f * std::max(0.0f, 0.4f - t));
    } else {
      const float t = (py - band_top) / std::max(1.0f, band_bottom - band_top);
      c = Mix(light, section, std::min(1.0f, t * 1.4f));
      // The swirl: a soft light band sweeping down from the upper left.
      const float d = u - (0.15f + 0.55f * t * t);
      c = Mix(c, kWhiteRgb, 0.30f * std::exp(-d * d / 0.010f));
      c = Mix(c, deep, 0.25f * std::pow(std::fabs(u - 0.5f) * 2.0f, 4.0f));
    }
    return Col(c);
  });
  Outline(dl, a, b, r, Col(Scale(section, 0.35f)), 1.6f);
  // A thin inner highlight just inside the edge.
  Outline(dl, ImVec2(a.x + 2.5f * s, a.y + 2.5f * s), ImVec2(b.x - 2.5f * s, b.y - 2.5f * s), r - 2.5f * s,
          IM_COL32(255, 255, 255, 70), 1.2f);

  // Title band: Back button, the blade's name, the clock and a Close button.
  const float fs_head = std::max(15.0f, 40.0f * s);
  const float fs_body = std::max(11.0f, 22.0f * s);
  const float fs_small = std::max(10.0f, 18.0f * s);
  const float pad = 30.0f * s;
  const float btn_h = 40.0f * s;
  const float by = y + (title_h - btn_h) * 0.5f;
  // Back
  {
    const char* label = "Back";
    const float bw = TextSize(Body(), fs_body, label).x + 52.0f * s;
    const ImVec2 p0(a.x + pad, by), p1(a.x + pad + bw, by + btn_h);
    const bool hot = hover_back_;
    GlossPill(dl, p0, p1, hot ? Mix(section, kWhiteRgb, 0.3f) : Scale(section, 0.85f), btn_h * 0.5f);
    const float cy = (p0.y + p1.y) * 0.5f;
    const float ax = p0.x + 18.0f * s;
    dl->AddLine(ImVec2(ax + 8 * s, cy - 8 * s), ImVec2(ax, cy), kWhite, 3.0f * s);
    dl->AddLine(ImVec2(ax, cy), ImVec2(ax + 8 * s, cy + 8 * s), kWhite, 3.0f * s);
    TextShadow(dl, Body(), fs_body, ImVec2(ax + 16 * s, cy - fs_body * 0.55f), kWhite, label, 1.2f * s);
    hits_.push_back({p0.x, p0.y, p1.x, p1.y, kHitBack, 0, "back"});
    const float tx = p1.x + 22.0f * s;
    TextShadow(dl, Head(), fs_head, ImVec2(tx, y + (title_h - fs_head) * 0.5f), kWhite, name, 1.8f * s,
               IM_COL32(0, 0, 0, 130));
  }
  // Close
  {
    const ImVec2 c1(b.x - pad, by), c0(b.x - pad - btn_h, by + btn_h);
    const ImVec2 p0(c0.x, by), p1(b.x - pad, by + btn_h);
    GlossPill(dl, p0, p1, hover_close_ ? Mix(section, kWhiteRgb, 0.3f) : Scale(section, 0.85f), btn_h * 0.5f);
    const ImVec2 cc((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    const float k = 8.0f * s;
    dl->AddLine(ImVec2(cc.x - k, cc.y - k), ImVec2(cc.x + k, cc.y + k), kWhite, 3.0f * s);
    dl->AddLine(ImVec2(cc.x - k, cc.y + k), ImVec2(cc.x + k, cc.y - k), kWhite, 3.0f * s);
    hits_.push_back({p0.x, p0.y, p1.x, p1.y, kHitClose, 0, "close"});
    const std::string clock = ClockText();
    const ImVec2 ts = TextSize(Body(), fs_body, clock);
    TextShadow(dl, Body(), fs_body, ImVec2(p0.x - 18.0f * s - ts.x, y + (title_h - ts.y) * 0.5f), kWhite, clock,
               1.2f * s);
    (void)c1;
  }

  // Legend band.
  {
    const guide::Item* sel_item = nullptr;
    if (level_ == 1 && !items.empty()) {
      sel_item = &items[static_cast<size_t>(std::clamp(item_sel_, 0, static_cast<int>(items.size()) - 1))];
    }
    struct Leg {
      ImU32 col;
      const char* letter;
      const char* text;
      uint32_t button;
    };
    std::vector<Leg> legs = {{kButtonA, "A", "Select", kA}, {kButtonB, "B", "Back", kB}};
    if (sel_item && !sel_item->cvar.empty() && Enabled(*sel_item)) legs.push_back({kButtonX, "X", "Default", kX});
    legs.push_back({kButtonY, "Y", "Close Guide", kY});
    const float gr = 15.0f * s;
    const float ly = y + h - legend_h * 0.5f + 2.0f * s;
    float lx = a.x + pad + 18.0f * s;
    for (const auto& leg : legs) {
      const float x0 = lx;
      ButtonGlyph(dl, ImVec2(lx + gr, ly), gr, leg.col, leg.letter);
      lx += gr * 2.0f + 9.0f * s;
      const ImVec2 ts = TextSize(Body(), fs_body, leg.text);
      TextShadow(dl, Body(), fs_body, ImVec2(lx, ly - ts.y * 0.5f), kWhite, leg.text, 1.2f * s);
      lx += ts.x + 30.0f * s;
      hits_.push_back({x0, ly - gr - 6 * s, lx - 18.0f * s, ly + gr + 6 * s, kHitLegend, static_cast<int>(leg.button),
                       std::string("legend:") + leg.letter});
    }
    const char* hint = editing_ ? "Enter: set   Esc: cancel" : "LB / RB or Tab: blades";
    const ImVec2 hs = TextSize(Body(), fs_small, hint);
    Text(dl, Body(), fs_small, ImVec2(b.x - pad - 14.0f * s - hs.x, ly - hs.y * 0.5f), IM_COL32(255, 255, 255, 200),
         hint);
  }

  // The grey-white brushed content panel, its sides following the blade.
  const float inset = 24.0f * s;
  const float px0 = a.x + inset, px1 = b.x - inset;
  const float py0 = y + title_h + 18.0f * s, py1 = y + h - legend_h - 14.0f * s;
  if (px1 - px0 > 40.0f * s && wv > 0.0f) {
    const float pr = 32.0f * s;
    Mesh(dl, ImVec2(px0 + 2 * s, py0 + 4 * s), ImVec2(px1 + 2 * s, py1 + 5 * s), pr, 1, 10.0f,
         [](float, float, int) { return IM_COL32(0, 0, 0, 60); });
    Mesh(dl, ImVec2(px0, py0), ImVec2(px1, py1), pr, 6, 1.5f, [&](float u, float v, int row) {
      Rgb c = Mix(kPanelTop, kPanelBottom, v);
      // Brushed: a fine per-row grain, and a soft vertical sheen.
      const float grain = (static_cast<float>(Hash(static_cast<uint32_t>(row) * 2654435761u) & 0xff) / 255.0f - 0.5f);
      c = Scale(c, 1.0f + 0.035f * grain);
      c = Mix(c, kWhiteRgb, 0.35f * std::exp(-std::pow((u - 0.35f) / 0.25f, 2.0f)) * (1.0f - v));
      return Col(c);
    });
    Outline(dl, ImVec2(px0, py0), ImVec2(px1, py1), pr, Col(Scale(section, 0.45f), 0.9f), 1.4f);
    Outline(dl, ImVec2(px0 + 2 * s, py0 + 2 * s), ImVec2(px1 - 2 * s, py1 - 2 * s), pr - 2 * s,
            IM_COL32(255, 255, 255, 160), 1.0f);
    DrawContent(dl, px0 + 26.0f * s, py0 + 14.0f * s, px1 - 26.0f * s, py1 - 14.0f * s, items);
  }
}

void GuideDialog::DrawContent(ImDrawList* dl, float x0, float y0, float x1, float y1,
                              std::vector<guide::Item>& items) {
  const float s = s_;
  const float fs_body = std::max(11.0f, 22.0f * s);
  const float fs_small = std::max(10.0f, 18.0f * s);
  const float fs_sub = std::max(12.0f, 27.0f * s);
  const Rgb section = blades_.empty() ? SectionColour("Game", 0) : SectionColour(blades_[blade_].name, blade_);
  const ImU32 accent = Col(Scale(section, 0.75f));

  // Heading: where we are.
  std::string heading;
  if (!blades_.empty()) {
    const auto& blade = blades_[blade_];
    if (level_ == 1 && !blade.entries.empty()) {
      const int sel = std::clamp(entry_sel_[blade_], 0, static_cast<int>(blade.entries.size()) - 1);
      heading = blade.entries[static_cast<size_t>(sel)].title;
    } else {
      heading = blade.name;
    }
  }
  Text(dl, Head(), fs_sub, ImVec2(x0 + 6 * s, y0), accent, heading);
  const float list_top = y0 + fs_sub + 14.0f * s;
  dl->AddLine(ImVec2(x0, list_top - 6 * s), ImVec2(x1, list_top - 6 * s), Col(Scale(section, 0.7f), 0.5f), 1.2f);

  const float list_w = (x1 - x0) * 0.60f;
  const float lx0 = x0, lx1 = x0 + list_w;
  const float dx0 = lx1 + 26.0f * s, dx1 = x1;
  const float row_h = 48.0f * s;
  row_h_ = row_h;

  // Rows.
  struct Row {
    std::string label;
    bool enabled = true;
  };
  std::vector<Row> rows;
  int sel = 0;
  if (!blades_.empty() && level_ == 0) {
    for (const auto& e : blades_[blade_].entries) rows.push_back({e.title, true});
    sel = entry_sel_[blade_];
  } else {
    for (const auto& it : items) rows.push_back({it.label, Enabled(it)});
    sel = item_sel_;
  }
  const int n = static_cast<int>(rows.size());
  sel = n > 0 ? std::clamp(sel, 0, n - 1) : 0;
  const float view_h = y1 - list_top;
  const float content_h = row_h * static_cast<float>(n);
  overflow_ = content_h > view_h + 0.5f;
  if (ensure_visible_ && n > 0) {
    const float sy = row_h * static_cast<float>(sel);
    if (sy < scroll_) scroll_ = sy;
    if (sy + row_h > scroll_ + view_h) scroll_ = sy + row_h - view_h;
  }
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content_h - view_h));

  if (n == 0) {
    Text(dl, Body(), fs_body, ImVec2(lx0 + 10 * s, list_top + 12 * s), kTextGrey, "Nothing here yet");
  }
  dl->PushClipRect(ImVec2(lx0 - 60.0f * s, list_top), ImVec2(lx1 + 60.0f * s, y1 + 1.0f), true);
  for (int i = 0; i < n; ++i) {
    const float ry0 = list_top + row_h * static_cast<float>(i) - scroll_;
    const float ry1 = ry0 + row_h;
    if (ry1 < list_top || ry0 > y1) continue;
    const bool selected = i == sel;
    const ImVec2 p0(lx0, ry0 + 3 * s), p1(lx1, ry1 - 3 * s);
    if (selected) {
      GlossPill(dl, p0, p1, section, (p1.y - p0.y) * 0.5f);
    } else if (i == hover_row_) {
      dl->AddRectFilled(p0, p1, Col(section, 0.15f), (p1.y - p0.y) * 0.5f);
    }
    if (!selected && i + 1 < n && i + 1 != sel) {
      dl->AddLine(ImVec2(lx0 + 18 * s, ry1), ImVec2(lx1 - 18 * s, ry1), IM_COL32(150, 156, 164, 90), 1.0f);
    }
    const ImU32 tc = !rows[i].enabled ? kTextDisabled : selected ? kWhite : kText;
    const float ty = (ry0 + ry1) * 0.5f - fs_body * 0.55f;
    const float label_x = lx0 + 20.0f * s;
    if (selected) {
      TextShadow(dl, Body(), fs_body, ImVec2(label_x, ty), tc, rows[i].label, 1.0f * s, IM_COL32(0, 0, 0, 90));
    } else {
      Text(dl, Body(), fs_body, ImVec2(label_x, ty), tc, rows[i].label);
    }
    const float vy = (ry0 + ry1) * 0.5f;
    const float vx1 = lx1 - 18.0f * s;
    const ImU32 vc = selected ? kWhite : kTextGrey;
    std::string hit_name = "row:" + rows[i].label;
    if (level_ == 0) {
      // Entries open a page.
      const float cx = vx1 - 6 * s;
      dl->AddLine(ImVec2(cx - 7 * s, vy - 8 * s), ImVec2(cx, vy), vc, 2.5f * s);
      dl->AddLine(ImVec2(cx, vy), ImVec2(cx - 7 * s, vy + 8 * s), vc, 2.5f * s);
    } else {
      guide::Item& item = items[static_cast<size_t>(i)];
      switch (item.type) {
        case guide::ItemType::kToggle: {
          const bool on = item.get_bool && item.get_bool();
          const float bs = 24.0f * s;
          const ImVec2 b0(vx1 - bs, vy - bs * 0.5f), b1(vx1, vy + bs * 0.5f);
          dl->AddRectFilled(b0, b1, on ? Col(Mix(section, kWhiteRgb, selected ? 0.55f : 0.0f)) : IM_COL32(255, 255, 255, 230),
                            5.0f * s);
          dl->AddRect(b0, b1, selected ? kWhite : IM_COL32(120, 126, 134, 255), 5.0f * s, 0, 1.4f);
          if (on) {
            const ImU32 mc = selected ? Col(Scale(section, 0.6f)) : kWhite;
            dl->AddLine(ImVec2(b0.x + 5 * s, vy), ImVec2(b0.x + 10 * s, vy + 6 * s), mc, 3.0f * s);
            dl->AddLine(ImVec2(b0.x + 10 * s, vy + 6 * s), ImVec2(b1.x - 4 * s, vy - 7 * s), mc, 3.0f * s);
          }
          const std::string t = on ? "On" : "Off";
          const ImVec2 ts = TextSize(Body(), fs_small, t);
          Text(dl, Body(), fs_small, ImVec2(b0.x - 10 * s - ts.x, vy - ts.y * 0.5f), vc, t);
          break;
        }
        case guide::ItemType::kChoice: {
          const std::string t = ValueText(item);
          const ImVec2 ts = TextSize(Body(), fs_small, t);
          const float aw = 22.0f * s;
          const float tx = vx1 - aw - ts.x;
          Text(dl, Body(), fs_small, ImVec2(tx, vy - ts.y * 0.5f), vc, t);
          if (selected || i == hover_row_) {
            const float ax_r = vx1 - aw * 0.5f, ax_l = tx - aw * 0.5f;
            dl->AddTriangleFilled(ImVec2(ax_r - 4 * s, vy - 7 * s), ImVec2(ax_r + 4 * s, vy), ImVec2(ax_r - 4 * s, vy + 7 * s), vc);
            dl->AddTriangleFilled(ImVec2(ax_l + 4 * s, vy - 7 * s), ImVec2(ax_l - 4 * s, vy), ImVec2(ax_l + 4 * s, vy + 7 * s), vc);
            hits_.push_back({ax_l - aw * 0.6f, ry0, ax_l + aw * 0.6f, ry1, kHitPrev, i, "prev:" + item.label});
            hits_.push_back({ax_r - aw * 0.6f, ry0, ax_r + aw * 0.6f, ry1, kHitNext, i, "next:" + item.label});
          }
          break;
        }
        case guide::ItemType::kSlider: {
          const double v = drag_row_ == i ? drag_value_ : (item.get_value ? item.get_value() : 0.0);
          std::string t = FormatValue(item, v);
          if (editing_ && edit_row_ == i) t = edit_text_ + "_";
          const ImVec2 ts = TextSize(Body(), fs_small, t);
          const float tw = std::max(ts.x, 54.0f * s);
          const float bar_w = 130.0f * s;
          const float bx1 = vx1 - tw - 14.0f * s, bx0 = bx1 - bar_w;
          const float frac = item.max > item.min ? static_cast<float>((v - item.min) / (item.max - item.min)) : 0.0f;
          const float bh = 8.0f * s;
          dl->AddRectFilled(ImVec2(bx0, vy - bh * 0.5f), ImVec2(bx1, vy + bh * 0.5f), IM_COL32(150, 156, 164, 200), bh * 0.5f);
          dl->AddRectFilled(ImVec2(bx0, vy - bh * 0.5f), ImVec2(bx0 + (bx1 - bx0) * std::clamp(frac, 0.0f, 1.0f), vy + bh * 0.5f),
                            selected ? kWhite : Col(section), bh * 0.5f);
          const ImVec2 knob(bx0 + (bx1 - bx0) * std::clamp(frac, 0.0f, 1.0f), vy);
          dl->AddCircleFilled(ImVec2(knob.x, knob.y + 1.5f * s), 10.0f * s, IM_COL32(0, 0, 0, 70), 20);
          dl->AddCircleFilled(knob, 10.0f * s, Col(Mix(section, kWhiteRgb, 0.35f)), 20);
          dl->AddCircle(knob, 10.0f * s, Col(Scale(section, 0.5f)), 20, 1.3f);
          if (editing_ && edit_row_ == i) {
            dl->AddRectFilled(ImVec2(vx1 - tw - 6 * s, vy - ts.y * 0.62f), ImVec2(vx1 + 4 * s, vy + ts.y * 0.62f),
                              IM_COL32(255, 255, 255, 255), 4.0f * s);
            Text(dl, Body(), fs_small, ImVec2(vx1 - ts.x, vy - ts.y * 0.5f), kText, t);
          } else {
            Text(dl, Body(), fs_small, ImVec2(vx1 - ts.x, vy - ts.y * 0.5f), vc, t);
          }
          hits_.push_back({bx0 - 4 * s, ry0, bx1 + 4 * s, ry1, kHitSlider, i, "slider:" + item.label});
          break;
        }
        case guide::ItemType::kAction: {
          const float cx = vx1 - 6 * s;
          dl->AddLine(ImVec2(cx - 7 * s, vy - 8 * s), ImVec2(cx, vy), vc, 2.5f * s);
          dl->AddLine(ImVec2(cx, vy), ImVec2(cx - 7 * s, vy + 8 * s), vc, 2.5f * s);
          break;
        }
        case guide::ItemType::kInfo: {
          const std::string t = ValueText(item);
          const ImVec2 ts = TextSize(Body(), fs_small, t);
          Text(dl, Body(), fs_small, ImVec2(vx1 - ts.x, vy - ts.y * 0.5f), vc, t);
          break;
        }
      }
    }
    // Rows go first so the controls inside them win (hits are tested topmost
    // first, so push the row before its controls: insert it before them).
    const float hy0 = std::max(ry0, list_top), hy1 = std::min(ry1, y1);
    if (hy1 > hy0) {
      size_t at = hits_.size();
      while (at > 0 && (hits_[at - 1].kind == kHitPrev || hits_[at - 1].kind == kHitNext ||
                        hits_[at - 1].kind == kHitSlider) &&
             hits_[at - 1].index == i) {
        --at;
      }
      hits_.insert(hits_.begin() + static_cast<long>(at), HitRect{lx0, hy0, lx1, hy1, kHitRow, i, hit_name});
    }
  }
  dl->PopClipRect();

  // Scroll bar.
  if (overflow_) {
    const float sx = lx1 + 8.0f * s;
    dl->AddRectFilled(ImVec2(sx, list_top), ImVec2(sx + 5 * s, y1), IM_COL32(0, 0, 0, 30), 3 * s);
    const float frac = view_h / content_h;
    const float th = std::max(20.0f * s, view_h * frac);
    const float ty = list_top + (view_h - th) * (scroll_ / std::max(1.0f, content_h - view_h));
    dl->AddRectFilled(ImVec2(sx, ty), ImVec2(sx + 5 * s, ty + th), Col(section, 0.8f), 3 * s);
  }

  // Description pane.
  dl->AddLine(ImVec2(dx0 - 13 * s, list_top), ImVec2(dx0 - 13 * s, y1), IM_COL32(150, 156, 164, 110), 1.0f);
  if (n == 0) return;
  std::string title, now, desc, foot;
  if (level_ == 0 && !blades_.empty()) {
    const auto& e = blades_[blade_].entries[static_cast<size_t>(sel)];
    title = e.title;
    desc = e.description;
    foot = "Select to open";
  } else if (level_ == 1) {
    const guide::Item& item = items[static_cast<size_t>(sel)];
    title = item.label;
    if (Adjustable(item) || item.type == guide::ItemType::kInfo) now = "Now: " + ValueText(item);
    desc = item.description;
    if (!Enabled(item)) {
      foot = "Not available right now";
    } else if (item.type == guide::ItemType::kSlider) {
      foot = "Left / Right, drag, or type a number";
    } else if (!item.cvar.empty()) {
      foot = "Setting: " + item.cvar;
    }
  }
  float ty = list_top + 4 * s;
  const float wrap = dx1 - dx0;
  Text(dl, Head(), fs_body, ImVec2(dx0, ty), kText, title, wrap);
  ty += TextSize(Head(), fs_body, title, wrap).y + 8 * s;
  if (!now.empty()) {
    Text(dl, Body(), fs_small, ImVec2(dx0, ty), accent, now, wrap);
    ty += TextSize(Body(), fs_small, now, wrap).y + 8 * s;
  }
  if (!desc.empty()) {
    Text(dl, Body(), fs_small, ImVec2(dx0, ty), kTextGrey, desc, wrap);
    ty += TextSize(Body(), fs_small, desc, wrap).y + 10 * s;
  }
  if (!foot.empty()) {
    const ImVec2 fs = TextSize(Body(), fs_small * 0.9f, foot, wrap);
    Text(dl, Body(), fs_small * 0.9f, ImVec2(dx0, std::max(ty, y1 - fs.y)), kTextDisabled, foot, wrap);
  }
}

void GuideDialog::PublishTargets(float display_w, float display_h) {
  if (display_w <= 0.0f || display_h <= 0.0f) return;
  std::lock_guard lock(g_targets_mutex);
  g_targets.clear();
  for (const auto& h : hits_) {
    if (h.name.empty()) continue;
    const float cy = (h.y0 + h.y1) * 0.5f;
    const ImVec2 l = Warp(h.x0, cy), r = Warp(h.x1, cy);
    g_targets[h.name] = {l.x / display_w, h.y0 / display_h, r.x / display_w, h.y1 / display_h};
  }
}
}  // namespace rex::ui
