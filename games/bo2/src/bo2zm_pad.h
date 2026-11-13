// The port's button combinations: the game's controller reads go through one hook (bo2zm_pad.cpp), which hands
// each read to the modules that have a combination and hides the buttons they used from the game.
//   Back + Up / Down    graphics preset one step higher / lower (bo2zm_preset.cpp)
//   Aim + Down          fly mode on / off, when the Extras combo is enabled (bo2zm_cheats.cpp)

#pragma once

#include <cstdint>

namespace bo2zm::pad {

// XINPUT_GAMEPAD button bits.
constexpr uint16_t kUp = 0x0001;
constexpr uint16_t kDown = 0x0002;
constexpr uint16_t kLeft = 0x0004;
constexpr uint16_t kRight = 0x0008;
constexpr uint16_t kBack = 0x0020;

struct Pad {
  uint16_t buttons;  // held now
  uint16_t before;   // held at the previous read of this controller
  bool aim;          // the left trigger (aim down sights; ZL on the Switch, L2 / LT on the PC)
  uint32_t user = 0; // controller that supplied this read; distinct from the engine local-client slot

  bool Pressed(uint16_t button) const { return (buttons & button) && !(before & button); }
};

}  // namespace bo2zm::pad
