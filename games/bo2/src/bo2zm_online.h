// Internet play from the home menu (ZOMBIES MULTIPLAYER / MULTIPLAYER, games/bo2/src/ui/menus.lua). The SDK does the
// networking (rex/system/online_link.h: the router's port forwarding by UPnP, the public address by STUN, hole
// punching through the matchmaking relay in rex/system/online_relay.h, and the System Link layer's online addresses);
// this module runs the menu's commands and tells the menu where things stand.
//   bo2zm_online host     prepare to host: forwarding and address, then a join code
//   bo2zm_online join     the keyboard for a code; a valid code prepares to join that host
//   bo2zm_online cancel   B while preparing
//   bo2zm_online off      SYSTEM LINK's own buttons (a LAN game): online play ends
// The join code (rex/system/join_code.h) is the host's public IPv4 address and a check byte: 8 characters, XXXX-XXXX.
#pragma once

#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::online {

// false: not this module's command.
bool RunCommand(std::string_view name, std::string_view argument);

// From the game's per-frame hook (bo2zm_autoexec.cpp): the menu's dvars bo2zm_online_ui, bo2zm_online_text_ui,
// bo2zm_online_code_ui and bo2zm_online_help_ui, when something changed.
void EveryFrame(PPCContext& ctx, uint8_t* base);

// At exit: the router's forwarding goes.
void Stop();

}  // namespace bo2zm::online
