// The player's name: EDIT USERNAME in Extras (the home menu only, app/src/ui/menus.lua). The game takes the console
// profile's name as the gamertag (XamUserGetName, XamUserGetSigninInfo) and shows it in lobbies and matches; the SDK
// names the profile "User". The port keeps the player's pick as user_name in bo2zm_settings.txt and gives it to the
// profile at start-up, before the game signs in. Editing opens the SDK's text input (rex/kernel/xam/text_input.h: a
// text box on PC, the console's keyboard on Switch); the new name counts from the next lobby or match. A rename keeps
// the same profile: its folder (the game's profile settings) is named by the profile's XUID, not by its name
// (ContentManager::ResolveGameUserContentPath).
#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::name {

// After the runtime exists and before the game runs (bo2zm_register.cpp OnPostSetup): the saved name, if any.
void ApplySaved();

// From the game's per-frame hook on the main thread (bo2zm_autoexec.cpp): takes a name the player just entered (saves
// it and renames the profile) and shows the name to the menu in the game dvar bo2zm_name_ui.
void EveryFrame(PPCContext& ctx, uint8_t* base);

// The console command bo2zm_edit_name from the menu (bo2zm_commands.cpp): opens the text input. false: not this
// module's command.
bool RunCommand(std::string_view name, std::string_view argument);

}  // namespace bo2zm::name
