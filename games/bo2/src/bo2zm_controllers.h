// Local play's controllers: who owns the front end, the game's player numbers on the Switch, and joining the Local
// lobby through the Switch's controller screen (bo2zm_controllers.cpp).
#pragma once

#include <cstdint>
#include <string_view>

#include <rex/runtime.h>

#include "bo2zm_pad.h"

namespace bo2zm::controllers {

// Switch: the game's players follow the system's player numbers. Call in OnPreSetup, before
// bo2zm::test_pad::WrapInput (a test's script pads keep their own assignment).
void WrapInput(rex::RuntimeConfig& config);

// What the pad hook (bo2zm_pad.cpp) does with one controller read: buttons the game must not see, and buttons it
// sees held although they are not (a join press made for the player).
struct PadEdit {
  uint16_t hide = 0;
  uint16_t add = 0;
};
// Any thread (every controller read).
PadEdit OnPad(const bo2zm::pad::Pad& pad);

// Main thread, once a frame: shows the controller screen a controller outside the lobby asked for, once every button
// is up, and makes the join presses for the players it added.
void EveryFrame();

// The menus' command (bo2zm_commands.cpp): "bo2zm_local_lobby 1 <controller>|0", the Local or System Link lobby
// opened for that controller, or closed. false: not this command.
bool RunCommand(std::string_view name, std::string_view argument);

}  // namespace bo2zm::controllers
