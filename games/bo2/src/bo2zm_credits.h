// The home screen's credits line: "Ported by: AlvaroEPena" with GitHub's mark beside it (games/bo2/src/ui/credits, drawn by
// tools/make_credits.py), in the bottom-right corner of the Zombies home menu, on the row of its button prompts.
//
// It is this port's own layer, not part of the game's UI: the native renderer draws it over the finished image in
// its output pass (bo2zm_native_targets.cpp), like the FPS counter, from images that go into the program as bytes.
// The game's menus never know about it, so nothing of the game's is changed or added. The menu Lua (menus.lua) says
// when the home menu is up with nothing over it, and only then is it drawn: a popup (Extras, CHECK FOR UPDATES), any
// other menu and a match hide it.
//
// Each screen size gets an image drawn at one pixel per pixel: 720 lines (handheld) and 1080 (docked) have their own,
// so the text stays sharp; other sizes (a PC window) take the nearer one.

#pragma once

#include <cstdint>
#include <vector>

namespace bo2zm::credits {

// The menu's command "bo2zm_credits <0|1>" (bo2zm_commands.cpp): 1 while the home menu is up with nothing over it,
// sent again every half second; 0 when something covers it or it closes. Without a 1 for 1.5 s (the menus were reset
// without closing it) the line goes away by itself.
void SetShown(bool shown);

// Both images in one RGBA picture (straight alpha), decoded on the first call; nullptr if they could not be.
struct Picture {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
  uint32_t rects[2][4] = {};  // each image's x, y, width and height in the picture: [0] 720 lines, [1] 1080
};
const Picture* Images();

// Where to draw on an output of width x height pixels now: which image, its top-left pixel and its opacity. false =
// nothing to draw (hidden, or faded out).
struct Placement {
  int image = 0;
  int x = 0;
  int y = 0;
  float alpha = 0.0f;
};
bool Place(uint32_t width, uint32_t height, Placement& out);

}  // namespace bo2zm::credits
