// Graphics presets (bo2zm_preset: lowest, very_low, low, medium, high or custom). See bo2zm_preset.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/ppc.h>

#include "bo2zm_pad.h"

namespace bo2zm::preset {

// From the dvar registration hooks: the game registered (or registered again) a dvar. kind: 0 bool, 1 int,
// 2 float, 3 enum (which Dvar_Register function was called). name and dvar are guest addresses.
void DvarRegistered(uint8_t* base, uint32_t name, uint32_t dvar, int kind);

// From the game's per-frame hook on the main thread (bo2zm_autoexec.cpp, sub_8240F058): applies the preset on
// each map load, after a change from the menu or the button combination, and checks it stays applied.
void EveryFrame(PPCContext& ctx, uint8_t* base);

// From the hook of sub_828A84F0 (the game sets sm_sunEnable and sm_spotEnable back to 1 every frame there): puts
// back the preset's value.
void AfterForceShadows(uint8_t* base);

// For the output pass (any thread): the on-screen note to draw now, as up to 20 characters packed 6 bits each,
// 5 per word from the low bits (codes in bo2zm_output_gamma_ramp.frag; 0 ends the text). false: no note.
bool OnScreenNote(uint32_t (&codes)[4]);
// The message centred at the top of the image (the output pass, from any frame): true while one shows, its 40 codes
// in `codes` (the same 6-bit font, 5 per word). The four cores' failsafe: "FAILSAFE: REVERTED TO 3 CORES" for 3 s when
// it switches them off (bo2zm_switch.h CoresSwitchedOff); MARK LOCATION's "LOCATION N SAVED".
bool OnScreenBanner(uint32_t (&codes)[8]);
// Shows `text` (up to 40 of: A-Z, 0-9, space, : . - % /) there from delay_s from now, for `seconds`; it replaces the
// message showing. Any thread.
void ShowBanner(std::string_view text, double delay_s, double seconds);
// A map's first visit (a new install or shader library) compiles its pipelines one after another, for a minute or more
// on a Switch, and their draws are skipped until each is ready, so the screen stays mostly black. The renderer calls
// NoteFirstVisitCompile after each pipeline it really compiles (its compile threads); a run of them in a match shows
// "FIRST VISIT - PREPARING GRAPHICS <count>" at the top. FirstVisitBuilding: such a run is going on (any thread).
void NoteFirstVisitCompile();
bool FirstVisitBuilding();

// The map loading or loaded, as the loading screen named it ("zm_buried zclassic processing": map, game type, start
// location; empty before the first), its location's label ("BURIED"; empty for a map with none) and the preset in
// effect ("VERY LOW"). Any thread.
std::string LoadedMap();
std::string LoadedLocation();
std::string PresetLabel();

// Renderer option of the current preset (any thread): textures with mips start this many levels lower.
uint32_t MipsMinus();

// The console command "bo2zm_preset <name or 0-5>" (bo2zm_commands.cpp), sent by the Graphics Settings menu.
void SetFromMenu(std::string_view value);
// The console command "bo2zm_gfx_<option> <level name or index>" from the Graphics Settings menu: that option's level,
// and the preset becomes custom. false: not one of the options.
bool SetOptionFromMenu(std::string_view option, std::string_view value);

// Map profiles (bo2zm_preset.cpp), from the menu Lua's console commands:
// "bo2zm_profile_map <map> <game type> <start location>", from the loading screen of the map about to play;
void MapLoading(std::string_view value);
// "bo2zm_profile <0-10>", the menus' MAP PROFILE (0 global, then each location; ignored in a match);
void SelectProfileFromMenu(std::string_view value);
// "bo2zm_profile_on <0|1|2>", ENABLE PROFILE for that location, or in a match the one being played (2 turns it the
// other way). An explicit choice releases a benchmark or environment/command-line GLOBAL override for this session.
void EnableProfileFromMenu(std::string_view value);
// The controller (bo2zm_pad.cpp): Back + Up / Down steps the preset. Returns the buttons the game must not see.
uint16_t OnPad(const bo2zm::pad::Pad& pad);

// The game's client is in a match (its cl_ingame dvar): also on a console that joined another's System Link game,
// where no server of its own runs and the server's entities stay empty. Any thread.
bool ClientInGame();

// The campaign program (bo2cp_autoexec.cpp), once a frame: whether its front-end level ("frontend") is the map
// loaded. While it is, the preset is capped at bo2zm_front_end_preset (see bo2zm_preset.cpp). Main thread.
void SetFrontEnd(bool on);
// Whether the campaign's front-end level is loaded and gets its own settings (not during bo2zm_front_end_test_s's turns
// with the player's preset; any thread), and its frame rate cap (bo2zm_front_end_fps_cap, 0 = none).
bool FrontEnd();
int FrontEndFpsCap();
// The campaign's front-end backdrop (bo2cp_direct_campaign): the renderer says when the scene is kept (still and
// settled), so the menus' black veil over it can fade out (menus.lua). Any thread.
void SetFrontEndSceneKept(bool kept);
// While the menus' veil covers the front end (the console command "bo2zm_input_block 1|0" from menus.lua), the game
// sees no controller input (bo2zm_pad.cpp); 15 s at most. Any thread.
void SetInputBlocked(bool blocked);
bool InputBlocked();
bool FrontEndSceneKept();

}  // namespace bo2zm::preset
