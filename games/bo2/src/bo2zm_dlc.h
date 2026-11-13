// DLC map packs: how the menus treat them (see bo2zm_dlc.cpp).

#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace bo2zm::dlc {

// Called once per frame from the game's main thread (the per-frame hook in bo2zm_autoexec.cpp).
void EveryFrame(PPCContext& ctx, uint8_t* base);

}  // namespace bo2zm::dlc
