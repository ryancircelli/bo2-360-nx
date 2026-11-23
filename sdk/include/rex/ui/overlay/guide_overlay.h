/**
 * @file        rex/ui/overlay/guide_overlay.h
 * @brief       The in-game Guide: a Blades-styled overlay opened by the
 *              controller's Guide button. Pages come from rex/ui/guide.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rex/input/input_system.h>
#include <rex/ui/guide.h>
#include <rex/ui/imgui_dialog.h>

struct ImFont;
struct ImFontAtlas;
struct ImDrawList;
struct ImVec2;

namespace rex::ui {

/// Sits between the pads and the guest (InputSystem::GuestGate). Watches the
/// Guide button on every guest poll, hides it from the guest (guide_button =
/// on), and zeroes the player that holds the Guide while it is open.
class GuideInputGate {
 public:
  /// on_open runs on the guest thread; it must only post work to the UI thread.
  explicit GuideInputGate(std::function<void(int user)> on_open);

  void Filter(uint32_t user, rex::input::X_INPUT_STATE& state);
  bool BlocksKeys(uint32_t user) const;

  /// The Guide opened for this player (0-3), or closed (-1).
  /// After a close the player's pad stays hidden until every button is up,
  /// so the B or Guide press that closed it never reaches the game.
  /// Opening also holds the keyboard and mouse (rex/input/mnk), closing
  /// releases them and logs what was held back.
  void SetHolder(int user);
  int holder() const { return holder_.load(std::memory_order_acquire); }

  /// The installable gate.
  rex::input::InputSystem::GuestGate MakeGate();

 private:
  std::function<void(int)> on_open_;
  std::atomic<int> holder_{-1};
  std::atomic<int> draining_{-1};
  std::array<bool, 4> was_down_{};
  std::array<bool, 4> fired_{};
  std::array<int64_t, 4> down_since_ms_{};
  // While open: guest polls of the holder's pad, and how many had input.
  std::atomic<uint64_t> held_polls_{0};
  std::atomic<uint64_t> held_input_polls_{0};
};

/// The Guide's clickable controls, by name ("blade:System", "row:Volume",
/// "back", "close", ...): x0, y0, x1, y1 in fractions of the window. Only
/// while the Guide is open. For scripted tests (kbm_test_script).
bool GuideTarget(std::string_view name, float rect[4]);

/// Loads the Guide's fonts into the ImGui atlas (call from the font setup).
void LoadGuideFonts(ImFontAtlas* atlas);

class GuideDialog : public ImGuiDialog {
 public:
  struct Hooks {
    /// Pads to read for navigation (UI thread). Usually InputSystem::SnapshotDevices.
    std::function<std::vector<rex::input::InputSystem::DeviceSnapshot>()> snapshot;
    /// Asked to close the Guide; must defer the destruction (not inside Draw).
    std::function<void()> request_close;
  };

  /// owner: the player whose pad drives the Guide (-1: any pad).
  GuideDialog(ImGuiDrawer* imgui_drawer, int owner, Hooks hooks);
  ~GuideDialog() override;

  /// Moves straight to a blade by name (tests, "open on page").
  void SelectBlade(std::string_view name);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Entry {
    size_t page = 0;
    std::string group;
    std::string title;
    std::string description;
  };
  struct Blade {
    std::string name;
    std::vector<Entry> entries;
  };
  enum Button : uint32_t {
    kUp = 1u << 0, kDown = 1u << 1, kLeft = 1u << 2, kRight = 1u << 3,
    kA = 1u << 4, kB = 1u << 5, kX = 1u << 6, kY = 1u << 7,
    kLB = 1u << 8, kRB = 1u << 9, kGuide = 1u << 10,
  };

  void Rebuild();
  std::vector<guide::Item> ItemsFor(const Entry& entry);
  std::vector<guide::Item> CurrentItems();
  uint32_t PollPads();
  uint32_t PollKeyboard();
  bool HandleNumberEntry(std::vector<guide::Item>& items);
  void HandleButtons(uint32_t pressed, std::vector<guide::Item>& items);
  void Activate(guide::Item& item, int direction);
  void SetSlider(guide::Item& item, double value);
  void SwitchBlade(int delta);
  void Back();
  void RequestClose();

  // Drawing (design units: a 700 px tall Guide).
  struct Span {
    float l, r;
  };
  std::vector<Span> Layout(size_t open, float x, float w) const;
  void DrawGuide(ImDrawList* dl, float x, float y, float w, float h,
                 std::vector<guide::Item>& items);
  void DrawClosedBlade(ImDrawList* dl, size_t i, Span span, bool left, float y, float h, bool own = false);
  void DrawOpenBlade(ImDrawList* dl, Span span, float y, float h, std::vector<guide::Item>& items);
  void DrawContent(ImDrawList* dl, float x0, float y0, float x1, float y1,
                   std::vector<guide::Item>& items);
  void PublishTargets(float display_w, float display_h);

  // The Blades bow: nominal x is pushed out from the centre line, most at
  // mid height, so straight edges draw as ( and ) curves.
  ImVec2 Warp(float x, float y) const;
  float UnwarpX(float x, float y) const;
  void WarpVertices(ImDrawList* dl, int first_vertex) const;

  int owner_;
  Hooks hooks_;
  std::vector<guide::Page> pages_;
  std::vector<Blade> blades_;
  size_t blade_ = 0;
  int level_ = 0;  // 0: the blade's entries, 1: an entry's items
  std::vector<int> entry_sel_;
  int item_sel_ = 0;
  float scroll_ = 0.0f;
  bool ensure_visible_ = true;
  uint32_t prev_buttons_ = 0;
  bool first_poll_ = true;
  int64_t repeat_at_ms_ = 0;
  uint32_t repeat_button_ = 0;
  bool close_requested_ = false;
  std::string pending_blade_;
  // Blade slide.
  size_t anim_from_ = 0;
  int64_t anim_start_ms_ = 0;
  // Typed number for a slider.
  bool editing_ = false;
  int edit_row_ = -1;
  std::string edit_text_;
  // Slider drag.
  int drag_row_ = -1;
  double drag_value_ = 0.0;
  float drag_x0_ = 0.0f, drag_x1_ = 0.0f;
  // Mouse.
  int hover_row_ = -1;
  bool hover_back_ = false;
  bool hover_close_ = false;
  // Last frame's list metrics (mouse wheel).
  float s_ = 1.0f;
  float row_h_ = 0.0f;
  bool overflow_ = false;
  // Warp.
  float warp_cx_ = 0.0f, warp_y0_ = 0.0f, warp_h_ = 1.0f, warp_k_ = 0.0f;
  // Mouse targets from the last frame, in nominal (unwarped) coordinates.
  enum HitKind : int {
    kHitBlade, kHitRow, kHitLegend, kHitPanel, kHitBack, kHitClose, kHitSlider, kHitPrev, kHitNext,
  };
  struct HitRect {
    float x0, y0, x1, y1;
    int kind;
    int index;
    std::string name;
  };
  std::vector<HitRect> hits_;
};

}  // namespace rex::ui
