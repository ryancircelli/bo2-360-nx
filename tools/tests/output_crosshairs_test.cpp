#include "../../games/bo2/src/native/render/bo2zm_output_crosshairs.h"
#include <cassert>
#include <limits>
#include <iostream>

struct Overlay { float normalizedRect[4]; bool enabled; int type; int color; };
int main() {
  using bo2zm::native::output::PackCrosshairs;
  Overlay records[5]{{{0, 0, .5f, .5f}, true, 0, 0},
                     {{.5f, 0, .5f, .5f}, true, 1, 1},
                     {{0, .5f, .5f, .5f}, true, 0, 2},
                     {{.5f, .5f, .5f, .5f}, true, 1, 3},
                     {{0, 0, 1, 1}, true, 1, 6}};
  auto packed = PackCrosshairs(records, 5);
  assert(packed.Any());
  for (int i = 0; i < 4; ++i) {
    assert(packed.styles[i][0] == 1 && packed.styles[i][1] == uint32_t(i % 2));
    assert(packed.styles[i][2] == uint32_t(i));
    const float x = (packed.rects[i][0] + packed.rects[i][2] / 2) * 1280;
    const float y = (packed.rects[i][1] + packed.rects[i][3] / 2) * 720;
    assert(x == (i % 2 ? 960 : 320) && y == (i < 2 ? 180 : 540));
  }
  // Insets stay relative to the full image, including black borders.
  Overlay inset{{.1f, .05f, .8f, .4f}, true, 1, 6};
  packed = PackCrosshairs(&inset, 1);
  assert(packed.rects[0][0] == .1f && packed.rects[0][1] == .05f);
  assert(packed.styles[0][2] == 6 && packed.styles[1][0] == 0);
  for (int bad = 0; bad < 7; ++bad) {
    Overlay invalid = inset;
    if (bad == 0) invalid.normalizedRect[0] = std::numeric_limits<float>::quiet_NaN();
    if (bad == 1) invalid.normalizedRect[2] = std::numeric_limits<float>::infinity();
    if (bad == 2) invalid.normalizedRect[1] = -.1f;
    if (bad == 3) invalid.normalizedRect[2] = 0;
    if (bad == 4) invalid.normalizedRect[3] = 2;
    if (bad == 5) invalid.type = 2;
    if (bad == 6) invalid.color = 7;
    assert(!PackCrosshairs(&invalid, 1).Any());
  }
  for (auto& record : records) record.enabled = false;
  packed = PackCrosshairs(records, 4);
  assert(!packed.Any());
  assert(!PackCrosshairs<Overlay>(nullptr, 4).Any());
  assert(!PackCrosshairs(records, -1).Any());
  std::cout << "Four independent output rectangles/styles, bounds, insets and cleared transitions passed\n";
}
