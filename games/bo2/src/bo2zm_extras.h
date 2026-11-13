// The port's own settings in the Extras menu (games/bo2/src/ui/menus.lua): the FPS counter and the cheats the game has no
// working dvar for (god mode, noclip, UFO, collision, the fly combo, jump height, the hidden HUD, third person;
// bo2zm_cheats.cpp).
// The game's built-in cheats (its cheat-protected dvars) are held by bo2zm_cheats.cpp. The port turns sv_cheats on as
// well (bo2zm_sv_cheats in bo2zm_autoexec.cpp).
//
// Adding a setting: a row in kOptions (bo2zm_extras.cpp) and an item in one of the menu's ExtrasPages.

#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::extras {

// From the game's main-frame hook: loads saved settings and publishes menu index
// values. Personal settings use bo2zm_p<controller>_<name>_ui; legacy
// bo2zm_<name>_ui follows the primary player. Only existing preferences are saved.
void EveryFrame(PPCContext& ctx, uint8_t* base);
// From the start of the game's per-frame hook: in a match with FPS CAP below 60, waits until the frame's time at that
// rate is up.
void PaceFrame();

// Compatibility commands without a controller default to the primary player.
// Values accept a menu index or a name (off/on, noclip/ufo).
bool RunCommand(std::string_view name, std::string_view argument);

// The command bridge resolves an active controller before calling this. Personal
// options and queued actions use that local actor; world actions require primary.
bool RunPlayerCommand(int local, std::string_view name, std::string_view argument);

}  // namespace bo2zm::extras
