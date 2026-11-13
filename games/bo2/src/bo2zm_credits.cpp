// See bo2zm_credits.h.

#include "bo2zm_credits.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include <rex/logging.h>
#include <rex/ui/image_decode.h>

#include "bo2zm_credits_png.h"  // generated from games/bo2/src/ui/credits (app/CMakeLists.txt)

namespace bo2zm::credits {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kHeartbeatNs = 1'500'000'000;  // a 1 lasts this long without the next
constexpr double kFadeInS = 0.25;
constexpr double kFadeOutS = 0.15;
// Where, in the home menu's 1280 x 720 layout: the line's middle on the row of the button prompts (A Select, B Back,
// whose middle is 672 lines down), its right end this far from the screen's right edge.
constexpr double kMiddleY = 672.0;
constexpr double kRightMargin = 48.0;

std::atomic<bool> g_shown{false};
std::atomic<int64_t> g_shown_since{0};  // Clock, ns: when the 1 came after a 0
std::atomic<int64_t> g_heartbeat{0};    // the last 1
std::atomic<int64_t> g_hidden_at{0};    // the last 0
std::atomic<float> g_alpha_at_hide{0.0f};  // the opacity then: the fade out starts from it

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// The fade in at time t (ns), from the last 1 after a 0.
float FadeIn(int64_t t) {
  const int64_t since = g_shown_since.load(std::memory_order_relaxed);
  return since == 0 ? 0.0f : float(std::clamp(double(t - since) / 1e9 / kFadeInS, 0.0, 1.0));
}

// How opaque the line is now: fades in after a 1, out after a 0 or a missed heartbeat (from where the fade in was: a
// popup that replaces another shows the menu for a moment, and must not flash the line).
float Alpha() {
  const int64_t now = Now();
  const int64_t heartbeat = g_heartbeat.load(std::memory_order_relaxed);
  if (g_shown.load(std::memory_order_relaxed)) {
    if (now - heartbeat < kHeartbeatNs) {
      return FadeIn(now);
    }
    const int64_t end = heartbeat + kHeartbeatNs;
    return FadeIn(end) * float(std::clamp(1.0 - double(now - end) / 1e9 / kFadeOutS, 0.0, 1.0));
  }
  const int64_t end = g_hidden_at.load(std::memory_order_relaxed);
  return g_alpha_at_hide.load(std::memory_order_relaxed) *
         float(std::clamp(1.0 - double(now - end) / 1e9 / kFadeOutS, 0.0, 1.0));
}

}  // namespace

void SetShown(bool shown) {
  const int64_t now = Now();
  if (shown) {
    if (!g_shown.exchange(true, std::memory_order_relaxed) ||
        now - g_heartbeat.load(std::memory_order_relaxed) >= kHeartbeatNs) {
      g_shown_since.store(now, std::memory_order_relaxed);
    }
    g_heartbeat.store(now, std::memory_order_relaxed);
  } else if (g_shown.load(std::memory_order_relaxed)) {
    g_alpha_at_hide.store(Alpha(), std::memory_order_relaxed);
    g_hidden_at.store(now, std::memory_order_relaxed);
    g_shown.store(false, std::memory_order_relaxed);
  }
}

const Picture* Images() {
  static Picture picture;
  static bool ok = false;
  static std::once_flag once;
  std::call_once(once, [] {
    int w[2] = {}, h[2] = {};
    const std::vector<uint8_t> images[2] = {rex::ui::DecodeImageRGBA(kPng720, sizeof(kPng720), w[0], h[0]),
                                            rex::ui::DecodeImageRGBA(kPng1080, sizeof(kPng1080), w[1], h[1])};
    if (images[0].empty() || images[1].empty()) {
      REXLOG_ERROR("[credits] the credits images could not be decoded: no credits line");
      return;
    }
    picture.width = uint32_t(std::max(w[0], w[1]));
    picture.height = uint32_t(h[0] + h[1]);
    picture.rgba.assign(size_t(picture.width) * picture.height * 4, 0);
    uint32_t top = 0;
    for (int i = 0; i < 2; ++i) {
      for (int row = 0; row < h[i]; ++row) {
        std::copy_n(images[i].data() + size_t(row) * w[i] * 4, size_t(w[i]) * 4,
                    picture.rgba.data() + (size_t(top + row) * picture.width) * 4);
      }
      picture.rects[i][0] = 0;
      picture.rects[i][1] = top;
      picture.rects[i][2] = uint32_t(w[i]);
      picture.rects[i][3] = uint32_t(h[i]);
      top += uint32_t(h[i]);
    }
    ok = true;
  });
  return ok ? &picture : nullptr;
}

bool Place(uint32_t width, uint32_t height, Placement& out) {
  const float alpha = Alpha();
  if (alpha <= 0.0f) {
    return false;
  }
  const Picture* picture = Images();
  if (!picture) {
    return false;
  }
  out.image = height > 900 ? 1 : 0;
  const uint32_t* rect = picture->rects[out.image];
  const double scale = double(height) / 720.0;
  out.x = int(width) - int(std::lround(kRightMargin * scale)) - int(rect[2]);
  out.y = int(std::lround(kMiddleY * scale)) - int(rect[3] / 2);
  out.alpha = alpha;
  return out.x >= 0 && out.y >= 0 && out.y + int(rect[3]) <= int(height);
}

}  // namespace bo2zm::credits
