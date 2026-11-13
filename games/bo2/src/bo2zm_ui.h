// This project's changes to the game's menus (see bo2zm_ui.cpp and games/bo2/src/ui/menus.lua).

#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace bo2zm::ui {

// Called once per frame from the game's main thread (the per-frame hook in bo2zm_autoexec.cpp).
void EveryFrame(PPCContext& ctx, uint8_t* base);

}  // namespace bo2zm::ui
