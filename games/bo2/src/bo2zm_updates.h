// CHECK FOR UPDATES on the home screen (games/bo2/src/ui/menus.lua), in builds for development consoles: it asks the PC that
// serves their builds what it stages for this console and compares that with the files here, without downloading
// anything (no update server set up, no server answered, up to date, or an update with its build and size). The
// download itself runs at the next start of the game, before the game exists, because the running game keeps files
// open that an update replaces. Standard builds have no update server: the menus leave the button out and these
// functions do nothing (bo2zm_lifecycle.cpp).

#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::updates {

// The menu's command "bo2zm_update <check|cancel|install>" (bo2zm_commands.cpp). check starts a check (a later one
// replaces a check under way), cancel forgets it (B on the screen), install relaunches into the update found.
void Command(std::string_view argument);

// From the game's per-frame hook on the main thread: shows the check to the menu in game dvars when it changed.
// bo2zm_update_ui is the state (the numbers in bo2zm_updates.cpp), bo2zm_update_text_ui its title and lines
// separated by '|', bo2zm_update_install_ui 1 when DOWNLOAD AND RESTART can run.
void EveryFrame(PPCContext& ctx, uint8_t* base);

// At shutdown: waits for a check under way (bounded by the socket timeouts).
void Stop();

}  // namespace bo2zm::updates
