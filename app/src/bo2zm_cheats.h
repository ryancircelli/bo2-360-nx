// Cheats for the Zombies modes, driven by the Extras menu (app/src/ui/menus.lua; the saved settings are in
// bo2zm_extras.cpp). bo2zm_cheats.cpp explains how each one works; in short:
//   - God mode: the primary local actor takes no damage at all (the game's damage function skips them, so nothing kills them, not
//     even leaving the playable area), and the game's own invulnerability flag is kept set.
//   - Noclip and UFO (fly modes, one at a time): retail TU18 has no code for them. Noclip runs the game's spectator
//     flying; UFO is our own smooth flight toward where the camera looks. One collision setting, shared by both,
//     keeps walls solid or lets the player fly through them.
//   - The combo: Aim + Down turns the chosen fly mode on or off (bo2zm_pad.cpp).
//   - Jump height and third person: the game ignores jump_height and cg_thirdPerson, so these change what it really
//     uses (its jump height constant, the third person byte). Feather fall slows falls and lets the stick steer them.
//     The hidden HUD is applied by the menu script (the HUD is a LUI menu); this code only keeps the setting.
//   - The game's built-in cheats (cheat-protected dvars): the menu asks for a dvar to be held at a value
//     ("bo2zm_hold <dvar> <value>", "bo2zm_release <dvar> <shown value>"). We set it, put it back whenever a map load
//     or a script changes it, restore the game's own value on release, and keep the pick in the game dvar
//     bo2zm_hold_<dvar> for the menu.
//   - Weapons, perks, power-ups, rounds, doors, power and debris: the game's own scripts, run on the Server thread
//     between server frames, limited to what this map has (the menu's catalog).
// Off-state hooks check player atomics before resolving actors. Held dvars are checked four times a second when
// any are held. Actor mappings and renderer snapshots are refreshed by the main frame.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/ppc.h>

#include "bo2zm_pad.h"
#include "bo2zm_player_cheat_state.h"

namespace bo2zm::cheats {

enum Fly { kFlyOff = 0, kNoclip = 1, kUfo = 2 };

// Local-client indices are engine view slots, not controller or server entity
// numbers. These cached lookups never call guest code; -1 means not mapped.
int LocalPlayerForController(int controller);
int PrimaryLocalPlayer();
bool IsPrimaryController(int controller);

// Per-local-player Extras options, in menu index units. Unknown/world options
// and invalid local slots return false. Stored picks survive CHEATS off.
bool GetPlayerOption(int local, std::string_view option, int& value);
bool SetPlayerOption(int local, std::string_view option, int value);
// Player commands capture their actor when queued. Shared world commands are
// accepted only for the primary local player.
bool RunPlayerCommand(int local, std::string_view name, std::string_view argument);
uint16_t OnPad(const bo2zm::pad::Pad& pad, int local);

struct CrosshairOverlay {
  float normalizedRect[4]{};  // x, y, width, height in the complete guest output image
  bool enabled = false;
  int type = 0;
  int color = 0;
};
// Coherent main-frame snapshot, ordered by local-client slot. Copies up to
// capacity records (maximum four) and returns that count. No guest reads/calls;
// inactive/invalid views are omitted, and transitions clear the snapshot.
int GetCrosshairOverlays(CrosshairOverlay* out, int capacity);
// Call before the server script begin-load (8252DED0), even when reloading the
// same map. Invalidates queued actor work before guest loading can reuse slots.
void BeginMapLoad();
// Call after normal return from server script startup (823599E8). Main-frame
// publication may then resolve fresh actors; it never restores old queues.
void EndMapLoad();
// Read-only effective settings for scoped projection/jump hooks. PS must be
// exactly an owned server/predicted state, never just a matching clientNum.
int JumpLevelForState(const uint8_t* base, uint32_t ps);  // 0..5; invalid/unowned => 0
float FieldOfViewScaleForLocal(int local);  // cached active actor; invalid/off => 1.0

// From the game's per-frame hook on the main thread (bo2zm_autoexec.cpp).
void EveryFrame(PPCContext& ctx, uint8_t* base);
// Right after each server frame, on the Server thread (the hook is in bo2zm_cheats.cpp): the cheats that run game
// scripts.
void ServerFrame(PPCContext& ctx, uint8_t* base);

// "bo2zm_hold <dvar> <value>", "bo2zm_release <dvar> <shown value>" and "bo2zm_reset_cheats" (every cheat back to
// normal play; the fly combo and collision settings stay) from the menu (bo2zm_commands.cpp). false: not one of these.
bool RunCommand(std::string_view name, std::string_view argument);

// The Extras menu's CHEATS switch (saved as "cheats", off at first). Off pauses the cheats: every cheat is back to
// normal play, the menu's cheat commands and the fly combo do nothing, and the picks are kept (the menu's held dvars
// here, the cheat settings in bo2zm_extras.cpp, which shows them); on again gives the picks back. RESET CHEATS clears
// kept picks too. The program's own uses of the cheats (the test helpers, the benchmark's round pick) do not go
// through it.
int MenuCheats();
void SetMenuCheats(int on);

// The controller (bo2zm_pad.cpp): Aim + Down for the combo. Returns the buttons the game must not see.
uint16_t OnPad(const bo2zm::pad::Pad& pad);

// State behind the Extras options (any thread).
bool GodMode();
void SetGodMode(bool on);
int FlyMode();  // Fly
void SetFlyMode(int mode);
bool FlyCollision();
void SetFlyCollision(bool on);
bool Combo();
void SetCombo(bool on);
int ComboMode();  // kNoclip or kUfo
// Test helper: "x y z" puts the players there; anything else only starts the origin log (see bo2zm_cheats.cpp).
void TestGoto(std::string_view argument);
void SetComboMode(int mode);
int JumpLevel();  // 0 normal .. 5 extreme
void SetJumpLevel(int level);
bool HudHidden();
void SetHudHidden(bool hidden);
bool ThirdPerson();
void SetThirdPerson(bool on);
int ThirdPersonCrosshairSetting();  // 0 off, 1 when aiming, 2 always
void SetThirdPersonCrosshair(int mode);
int CrosshairType();  // 0 dot, 1 small cross
void SetCrosshairType(int type);
int CrosshairColor();  // 0 white, 1 red, 2 green, 3 cyan, 4 yellow, 5 magenta, 6 orange
void SetCrosshairColor(int color);
// For the output pass (any thread): draw the third person crosshair now.
bool ThirdPersonCrosshair();
bool FeatherFall();
void SetFeatherFall(bool on);
bool InfiniteAmmo();
void SetInfiniteAmmo(bool on);
bool ExplosiveBullets();
void SetExplosiveBullets(bool on);
bool NoRecoil();
void SetNoRecoil(bool on);
bool ZombiesIgnore();
void SetZombiesIgnore(bool on);
bool StopSpawns();
void SetStopSpawns(bool on);
// Goes to a round (1..255): the current round ends and the game goes on to that one (see bo2zm_cheats.cpp).
void GoToRound(int round);
bool KeepPerksOn();
void SetKeepPerks(bool on);
// Player 0 is in a match (their client exists), as of the last frame. Any thread.
bool InMatch();
// The WEAPONS page (bo2zm_give_weapon <name>): give the Pack-a-Punch version (a preference, saved). The PERKS page:
// also list the perks of other maps' machines (a preference, saved; the menu Lua reads it). Any thread.
bool GivePackAPunch();
void SetGivePackAPunch(bool on);
bool CrossMapPerks();
void SetCrossMapPerks(bool on);
// The round the game shows (its round counter), as of the last frame; 0 outside a match. Any thread.
int CurrentRound();
// Where the primary local actor is, as of the last frame (MARK LOCATION, bo2zm_debug_options.cpp): the feet (the entity's origin),
// the camera (the view origin the last frame drew from) and the view angles in degrees (pitch, yaw, roll). false
// outside a match. Any thread.
struct Spot {
  float feet[3];
  float camera[3];
  float angles[3];
};
bool PlayerSpot(Spot& out);
// Test spots (bo2zm_test_spot, bo2zm_benchmark.cpp): until ReleaseHold, the primary local actor stands at `feet` looking at pitch and
// yaw (degrees) whatever the stick does; every movement step puts them back there. KillZombiesNow kills every zombie
// alive (as KILL ALL ZOMBIES does, whatever CHEATS is). Any thread.
void HoldAt(const float feet[3], float pitch, float yaw);
void ReleaseHold();
void KillZombiesNow();
// The server's parent script variables in use (of 24576), counted about once a second; -1 before a match. Any
// thread.
int ScriptParentsInUse();
// Field of view, a setting kept between runs (the cheats reset puts it back to 65): 0..11 = 55, 60, ..., 110
// degrees (2 = 65, the game's). The scoped projection hook consumes FieldOfViewScaleForLocal; this core does not
// write a personal FOV into a shared dvar. Raised jumps also require the companion scoped limit hooks.
int FieldOfView();
void SetFieldOfView(int level);

// System Link (bo2zm_cheat_sync.cpp). Main thread only.
// This console's local players as the menu set them: each one's controller and its synced options' effective
// values (detail::kSyncedOptions order). Up to capacity (at most four); the count.
struct SyncedPlayer {
  int controller = -1;
  int values[detail::kSyncedCount]{};
};
int LocalSyncedPlayers(SyncedPlayer* out, int capacity);
// The host's side: a player on another console asks for these options (detail::kSyncedOptions order, already
// checked against each option's range). They apply to the server's player with this XUID while it plays here, the
// way a local player's options apply; a remote player's state is forgotten on a map load or once it has left.
// false: refused (no free remote slot, or an older sequence than the last one taken). *client: the server client
// it maps to now, -1 when not (yet); *changed: a value differs from what it had.
bool SetRemotePlayer(const uint8_t* base, uint64_t xuid, uint32_t sequence, const int* values, int* client,
                     bool* changed);
// This console is in a Zombies match another console hosts (as of the last frame). Any thread.
bool JoinedRemoteGame();
// This console's own server runs (sv_running and a client on the first player entity).
bool LocalServer(const uint8_t* base);
// The host's side of a joining player's command (bo2zm_perk, bo2zm_give_weapon, bo2zm_powerup, bo2zm_doors,
// bo2zm_goto_round, bo2zm_hold of a world setting, ...): run for the remote player with this XUID, or for the match
// with that player as the one who asked. 0: done (or queued for the next server frame); otherwise refused, with why.
int RunRemoteCommand(const uint8_t* base, uint64_t xuid, std::string_view name, std::string_view argument,
                     bool pack_a_punch, std::string& why);
// The host's menu catalog (guns, perks, power-ups) as text, and its revision; false outside a match.
bool CatalogText(std::string& text, uint32_t& revision);
// A joining console: the menu's catalog dvars from the host's catalog text.
void ApplyCatalog(PPCContext& ctx, uint8_t* base, std::string_view text);
// The host's match-wide settings ("timescale=0.5;g_ai=0;zm_nospawn=1;": the held world dvars and stopped spawns).
std::string WorldStateText();
// A joining console: hold the host's world settings here too (the prediction reads gravity and sprint itself, and
// the menu shows the host's picks); an empty text gives them back to the game.
void ApplyWorldState(std::string_view text);
// Explosive bullet hits of remote players since the last call (the effect is drawn by the console the player plays
// on), and on that console, one hit to draw.
struct RemoteBlastHit {
  uint64_t xuid;
  float at[3];
};
int TakeRemoteBlasts(RemoteBlastHit* out, int capacity);
void PlayRemoteBlast(const float at[3]);
// MOVE SPEED, a per-player cheat: 0 normal (the game's 190) .. 3 fastest (400).
int MoveSpeed();
void SetMoveSpeed(int level);

}  // namespace bo2zm::cheats
