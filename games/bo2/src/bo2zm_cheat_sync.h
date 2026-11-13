// System Link cheat sync. A console that joined another console's System Link match runs no server, so the server's
// side of its players' cheats (god mode, ammo, perks, guns, rounds, doors and the rest) has to run on the host. This
// sends its players' options and commands to the host, which applies them to that player's server entity or to the
// match, and sends back its menu catalog, its match-wide settings and the explosions to draw. bo2zm_cheat_sync.cpp
// explains the messages.

#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::cheat_sync {

// From the cheats' main-frame hook (bo2zm::cheats::EveryFrame), after the actors are up to date.
void EveryFrame(PPCContext& ctx, uint8_t* base);

// A cheat command one of this console's players picked in a match another console hosts (bo2zm_perk,
// bo2zm_give_weapon <name>, bo2zm_doors open, bo2zm_hold timescale 0.5, ...): sent to the host with the next frame,
// which runs it. false: not one the host takes, or too many are waiting. Any thread.
bool Forward(int controller, std::string_view name, std::string_view argument, bool pack_a_punch);

// bo2zm_cheat_sync_trace: every 10 s, the state the cheats' frame sees (whether a map is still loading, whether this
// console has a local player on its server), from the top of the frame before anything can return early.
void TraceFrame(bool map_loading, bool local_actor);

}  // namespace bo2zm::cheat_sync
