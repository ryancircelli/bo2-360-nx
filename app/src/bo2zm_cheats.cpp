// Cheats for the Zombies modes; the interface the menus and console commands use is in bo2zm_cheats.h. Retail TU18
// ships none of the developer cheats, so each one here is built from the game's own code: a flag or field the game
// already honours, a hook around one of its functions, or one of its own script functions run on the server's script
// instance. The notes below explain how each cheat works and where its offsets and functions come from; the code
// further down refers back to them.
//
// Threads. Movement, camera and weapon cheats live in hooks on the game's functions and in the main thread's frame
// hook. Everything that runs game script (power-ups, perks, rounds, the gun giver, doors, power, debris, killing
// zombies) runs on the Server thread right after its server frame (sub_82336790, hooked below, which runs the script
// threads through sub_825444F8), so it never overlaps a running script. The game runs its scripts only on that thread:
// stack samples show the script VM, sub_8253ECC8, only under sub_82435B78 -> sub_82336790. Running them from the main
// thread is not safe: a round change that killed every zombie from there exhausted the server's parent script
// variables ("exceeded maximum number of parent server script variables") and the Server thread then faulted inside
// the script VM. Menu requests therefore wait for the next server frame; outside a match no server frame runs, and
// they are dropped.
//
// God mode: TU18 has no god command, so two mechanisms work together. The first is the bit the script method
// enableinvulnerability sets (its method table entry is at 0x82025648, the function at 0x8231F2A0): it ORs 1 into the
// client's flags word at client + 24 (the client pointer is at entity + 340, g_entities at 0x833D0640). The game clears
// it on spawn, so it is set again every frame. The second is a hook on the game's damage function (sub_82325CA8, target
// entity in r3) that returns at once for the selected player. It covers what the flag does not: the Zombies script that kills a
// player outside the playable area (player_out_of_playable_area_monitor), the maps' kill volumes and falls.
//
// Fly modes: the movement step PmoveSingle (sub_8269AE30, pm in r3) picks the movement by ps->pm_type (ps = pm + 0,
// pm_type at ps + 4) through a jump table in which noclip (2) and UFO (3) fall into the default case with normal
// walking: their code is not in retail. The spectator case (5) flies with the normal slide move, which collides through
// the trace mask at pm + 124. For the selected player (ps->clientNum at ps + 416) in normal movement (pm_type 0):
//   - noclip runs the step as a spectator, with the spectator's own controls (the stick, plus up and down);
//   - UFO is our own flight. The left stick accelerates the player toward where the camera looks, pitch included (view
//     angles at ps + 504; ps + 152 holds the delta angles), and the velocity eases off when the stick is released. The
//     velocity lives in ps->velocity (ps + 52) like the game's, so the client's prediction, which runs the same
//     function on its copy, flies the same way. The game's step still runs, as a spectator with the stick and buttons
//     hidden: with collision it moves the player by that velocity through its slide move, which stops at walls;
//     without collision it moves nothing and we move the origin (ps + 40) ourselves. The step time is the command's
//     time (pm->cmd at pm + 4) minus ps->commandTime (ps + 0).
// Turning collision off also sets the trace mask to 0 for the step. Everything changed is restored right after the
// step, so the rest of the game sees a normal player. That includes the view height: for a spectator the stance step
// (sub_82698DC0, called from the spectator case) sets the view height target (ps + 516, an int) to 0 and the view
// height (ps + 520, a float) to 0.0 (the constant at 0x82000DC4), which would drop the camera to the player's feet the
// moment a fly mode came on. We keep both as they were before the step, so flying starts at eye height.
//
// Jump height: the jump uses the float constant 39.0 at 0x82085654, not the jump_height dvar (which only takes it as
// its default), and every reader of that constant is jump code. The shared constant remains at39; scoped limit
// hooks select JumpLevelForState for the exact owned PS. The game's jump start (sub_82687E20, pm in
// r3) sets velocity z to sqrt(2 * gravity * height) at once, which at the raised levels is a launch (about 6300 units/s
// at the highest), stores the start height at ps + 196 and sets bit 0x4000 of pm_flags (ps + 12); each step then keeps
// the rise speed under what the rest of the height needs. To make raised jumps smooth, the selected player's rise starts at the
// game's normal jump speed instead, and after each step it is set from the height already climbed: it speeds up at
// kJumpBoost to at most kJumpCruise, then slows with gravity for the last part (the game's own curve there). Falls are
// capped at kJumpCruise too. The speed depends only on the player state, so the client's prediction replays it
// exactly.
//
// Feather fall: after a normal movement step in the air, the selected player falls no faster than kFeatherFall and the stick
// steers them (up to kGlideSpeed), so a high jump becomes a glide.
//
// Hidden HUD: the menu script (app/src/ui/menus.lua) hides the HUD menu itself; this file only keeps the setting.
//
// Third person: the game's own, the byte at client + 132 that the script method setclientthirdperson sets. We keep it
// at 1 every frame and hold r_dof_enable at 0 meanwhile (aiming blurs the scene behind the sights, which in third
// person is the player), and the output pass draws a crosshair (ThirdPersonCrosshair, bo2zm_native_targets.cpp).
//
// Over-the-shoulder aim: while aiming (the left trigger) in third person, the camera comes closer, moves to the right
// and looks at what the player aims at. The game's third person view (sub_8223A410, local client in r3) puts the view
// origin (cg + 0x4D8B8, where cg = *0x82BBAE68 + local client * 0x87700) at the player model's head, builds the camera
// kCameraLift above it and cg_thirdPersonRange (its dvar pointer at 0x82BBC584) behind it along the camera's angles,
// traces to it with collision, and writes the camera to the view origin and its axis to cg + 0x4D8C8. Its input view
// angles are at cg + 0x64920; the camera backs off along half the view pitch (the double 0.5 at 0x82003400) and looks
// at a point 512 units ahead of the head along the view. We lower the range for the call, then find the head again
// from the camera by the same construction and trace the aim from there along the view with the bullets' mask. That
// has to be a line trace (TraceShot): the camera trace keeps a 12-unit box clear and lifts its result, which would put
// the aim point about 30 units too high. The camera then moves right and slightly down with the camera trace
// (sub_821B2AB0: cg, start, end, mask, out, 1, 1; it stops at walls) and turns to the aim point, so the centre of the
// screen, where the crosshair is, is where the shots go. An aim point too close to turn to safely falls back to the
// game's own point 512 units ahead. Everything eases in and out with the aim. Note that the client's input angles are
// not the view's: the view adds delta_angles, a fixed turn from the spawn.
//
// Field of view: a saved setting. cg_fovScale is held at the chosen degrees / 65 (the game's cg_fov is 65, and the
// game sets cg_fov itself). Resetting the cheats puts it back to 65 together with the other held dvars, so the setting
// and the menu never disagree with the game.
//
// Infinite ammo: player_sustainAmmo is not read in TU18. Every frame we refill the current weapon's clip the way the
// script method setweaponammoclip does. The weapon is the word at client + 440, its low byte the weapon index (0 or 255
// for none; getcurrentweapon, valid while client + 21520, the session state, is 0); sub_826BDC70(weapon) is its clip
// size and sub_826BDAF8(client, weapon, amount) sets the clip. A dual-wield weapon's left-hand gun (Mustang & Sally,
// the dual Five-seven) is a weapon of its own with its own clip, which the scripts fill the same way (setweaponammoclip
// on weapondualwieldweaponname). In the variant table its name is the right-hand one's with "lh" added
// (m1911_upgraded_zm -> m1911lh_upgraded_zm) or with "dw" turned into "lh" (fivesevendw_zm -> fivesevenlh_zm).
//
// Max points: the script field "score" is the client's word at client + 0x55C8 (the client field table entry at
// 0x820227F0); GIVE MAX POINTS (bo2zm_max_points) sets it to kMaxPoints once, on the next frame.
//
// Explosive bullets: bg_forceExplosiveBullets is not read in TU18. A bullet hit (sub_82300AF0: trace in r5 with the hit
// point at +68, weapon in r6, attacker entity in r7) explodes only for weapons with bulletImpactExplode, through the
// game's radius damage sub_823270B0 (origin, inflictor, attacker, f1 inner damage, f2 outer damage, f3 radius, f4 cone
// cosine, r10 cone direction; on the stack the entity to spare, the means of death and the weapon; the script's
// radiusdamage calls it the same way). After each of the selected player's hits we call it at the hit point, sparing the selected player,
// and on the next frame the client plays the frag grenade's own explosion there: its weapon's projExplosionEffect
// through sub_822DD668 (effect, origin, axis; every local client) and its projExplosionSound through sub_82584128
// (alias by name) and sub_82230800 (local client, entity, origin, 0, 0, f1 volume, r9 alias), as the client's
// projectile explosion does.
//
// No recoil: sub_826BAF08 (ps in r3, called by both the client's and the server's weapon fire) adds the shot's kick to
// the two angles at r4 (where the shot moves the aim) and the view kick at r5. For the selected player we restore both after the
// call.
//
// Zombies: a game entity (g_entities at 0x833D0640, 796 bytes each) has its client at +340, its actor at +344
// (zombies), its sentient at +348, its health at +424 and its origin at +308.
//   - Ignore players: the script field "ignoreme" is the byte at sentient + 0x14 (the sentient field table is at
//     0x8206E480). The zombie scripts skip players with it set, so we keep the selected player's at 1.
//   - Freeze is the game's g_ai, held by the menu: the actor think waits while it is 0.
//   - Stop spawns: the script method spawnactor (sub_823696B8, in the method table) does nothing, so the script gets no
//     zombie and tries again after its spawn delay.
//   - Kill all: each live actor takes the explosive bullets' radius damage at its origin, from the selected player. The kills run
//     the zombie scripts at once (a death can drop a power-up, which sets client fields), so this runs with the
//     server's client field registry, like the game scripts below.
//
// Game scripts: we run the game's own script functions on the server's script instance (0). sub_8252FCB0(instance,
// file, function, 0, 1) finds a function's code by name in a loaded script (or returns the value at 0x829917FC when the
// script has no such export). Arguments are pushed last first, with sub_8253D9C8 (a string), sub_8253DB48 (a vector),
// sub_8253D758 (an int) and sub_8253D7B8 (a float, in f1); sub_82543150(instance, function, count) runs the function
// as a thread, and sub_8253D3F8(instance, thread) releases the thread. Client fields (the script method setclientfield,
// used for example by the power-ups' glow) find their registry through a per-thread pointer, the word at (the guest r13
// block) + 6720. The server points it at its own registry, 0x8352C5F0, around client field work, and we do the same
// around our calls; without it the field lookup reads address 4.
//   - The developer script's functions (maps/mp/zombies/_zm_devgui) do nothing in retail: their whole bodies are
//     developer blocks (every call in them carries the developer flag 0x10 in the script's imports, where other
//     scripts' calls have 0x02), which the game skips with developer off. None of the cheats goes through them.
//   - Power-ups: maps/mp/zombies/_zm_powerups::specific_powerup_drop(name, spot), the game's own drop for scripted
//     rewards, spawns the power-up 40 units above spot and starts its pickup (zombie_devgui_give_powerup is a
//     developer function and drops nothing). A name the map has no power-up for stops the script with a non-fatal
//     error. Each power-up is a game entity until it is picked up or times out, and G_Spawn (sub_82389FB8) ends the
//     match with "G_Spawn: no free entities" when none is left: it hands out entities up to 1022 (the count at
//     level + 12, level at 0x83547A00), then reuses freed ones (a list from level + 16, linked through entity + 780).
//     So a power-up spawns only while at least kSpareEntities are free.
//   - Rounds: we do what the developer function zombie_devgui_goto_round would, with the script variables themselves.
//     The round loop (_zm::round_think) waits in round_wait while a zombie is alive or level.zombie_total > 0, then
//     works out the zombies' speed from level.round_number, adds 1 to it, shows it (setroundsplayed) and, after the
//     pause between rounds, starts the round: round_spawning sets that round's health and zombie count. So to go to
//     round N, once a round is on (zombie_total > 0 or a zombie alive; between rounds the request waits), we kill the
//     zombies, set level.zombie_total to 0 and level.round_number to N - 1, and the game ends the round and goes to
//     round N its own way. Picking the round already on (between rounds, the one coming) does nothing. While a change
//     is under way (until the game's round counter at level + 12176 shows it, or kRoundChangeTimeout passes) later
//     picks wait, and the same round again does nothing, so pressing A repeatedly changes the round once.
//   - Level fields: a loaded script's header comes from sub_8252EDC0(instance, file, 1, 1), which the function lookup
//     uses too. Its string fixups ({u16 string, u8 count, u8 type} followed by count code offsets, at header + 0x18,
//     count at header + 0x32; type 1 is a field name, type 0 a string) point at the operands the game linked to the
//     name's id. The level object is the word at 0x840DF080 + 0x1C (the script's "level", opcode 13).
//     sub_82534430(instance, object, name id) finds a field's variable and returns 0 when there is none. sub_82534918,
//     which "level.<field> =" uses through sub_82535418, also creates a missing one, so it must never be used just to
//     look: a scan with it created a field on every object and crashed the script VM on the next server frame. Both
//     handle object types up to 0x12 (we check the type first: the low 7 bits of the word at
//     (*0x840DEC80) + object * 8 + 4). A variable's value is at (*0x840DEE00) + variable * 16 with its type byte at
//     + 6 (7 for an int), as the store opcode (40) writes it.
//
// Perks: maps/mp/zombies/_zm_perks::give_perk(perk, bought), run on the selected player with the entity thread runner
// sub_8237CA30(entity, function, count), gives a perk the game's way (HUD icon, health, its threads). The engine's
// perk bits are two words at player state + 1352 (the bit of perk index i is 1 << (i & 31) in word i >> 5; BG_HasPerk,
// sub_821BED00), and sub_82690E50(name) is a perk's index (63 for none).
//   - A perk the map has no machine for (PhD Flopper in Town, for example) has no player client fields registered for
//     its HUD, and the script method setclientfieldtoplayer (codesetplayerstateclientfield, sub_823743A8) uses the
//     field lookup sub_821A5BE0(5, name, 1) without checking the result: a field the map never registered crashes it
//     (a guest read of 0x14). Our hook skips the update when the lookup finds nothing.
//   - Keep perks: perk_think removes a player's perks when they go down unless the script field _retain_perks is set,
//     which nothing sets in Town. So while the player is up we note every perk they have, and when they are up again
//     after going down (revived, Quick Revive in solo, or a respawn) we give back the ones they no longer have. A perk
//     that goes away while the player stays up (REMOVE PERKS, or the game's own doing) is forgotten.
//   - Remove perks: perk_think also ends a perk on the player's notify "<perk>_stop" (the way lose_random_perk does
//     it), so common_scripts/utility::delay_notify("<perk>_stop", 0.05) on the selected player takes each perk they have; the
//     float goes in with sub_8253D7B8.
//
// The menu's catalog: every 2 s in a match the main thread works out what the WEAPONS, PERKS and POWER UPS pages offer
// and publishes it in game dvars.
//   - Guns: the weapons the level registered (bg_weaponVariantDefs: variant + 0 is the internal name, + 0xC the display
//     name's localization key, + 8 the weapon def, with inventoryType at + 0x2C, 0 for a primary, and weaponClass at
//     + 0x20) that are primary guns, by category, each with whether its Pack-a-Punch version is registered too. The
//     engine registers a weapon only while the level starts and only when its files are loaded
//     (G_GetWeaponIndexForName, sub_823AB8B8: sub_823AB890 registers through sub_826C0598 only while level + 64 is
//     set, and that refuses a weapon whose asset is the default one; afterwards sub_826C00E0 only finds registered
//     ones). So this list is everything a match can give: a gun that lives in another map's files cannot be given.
//   - Perks: the map's perk machines, read from the "zombie_vending" triggers' script_noteworthy (entity + 0x17A is the
//     targetname and + 0x17C the script_noteworthy, both string ids, whose text SL_ConvertToString (sub_82532A18)
//     finds at *0x83EEF324 + id * 24 + 4; an entity is in use when the byte at entity + 251 is set, as getentarray
//     (sub_8237CCB0) checks), plus the perks the map turns on with level.zombiemode_using_<perk>_perk (Origins' random
//     perk machine sells perks that have no machine of their own).
//   - Power-ups: each map's include_powerups() list, read from its zone (kMapPowerups).
//
// The gun giver: on the Server thread between server frames, _zm_weapons::weapon_give(weapon, is_upgrade) on the selected player
// (what the box and the wall buys use), in place of the gun in their hands. That gun is taken first the way the
// takeweapon method does it (sub_82313568: client + 440 and + 21572 cleared, then sub_826B42B8, the player state's own
// take; has-weapon is sub_821A8C38). PACK-A-PUNCH CURRENT WEAPON gives the gun's upgraded version the same way.
//
// Doors, power and debris: OPEN ends each closed door trigger's door_think and runs _zm_blockers::door_opened on it,
// which skips the purchase whatever the map's own purchase logic (Mob of the Dead's Afterlife doors included). CLOSE
// runs door_opened on the open doors (the game's own closing, its door_can_close mode) and then door_think again, so
// they can be bought (or shocked) again the game's way. Debris and airlocks are bought for free: the points are lent
// for a server frame. POWER ON and OFF use _zm_game_module::turn_power_on_and_open_doors and
// common_scripts/utility::flag_clear("power_on"); on Origins, which has generators instead of a switch, every
// generator is also captured or lost, with the generator's struct as self (RunObjectThread). REMOVE FREE DEBRIS hides
// Buried's barricades that Leroy breaks with _zm_ai_sloth's own hide_sloth_barrier, and PUT DEBRIS BACK shows them
// again with unhide_sloth_barrier. The details are with OpenDoors, SetPower and SetDebris.

#include "bo2zm_cheats.h"
#include "bo2zm_player_cheat_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/system/xsocket.h>

#include "bo2zm_autoexec.h"
#include "bo2zm_cheat_sync.h"
#include "bo2zm_dvars.h"
#include "bo2zm_guest.h"
#include "bo2zm_mode.h"
#include "bo2zm_preset.h"

REXCVAR_DEFINE_STRING(bo2zm_test_cheats, "", "BO2",
                      "Test helper: \"seconds:commands|seconds:commands\", each run that many seconds after player 1 "
                      "is first in a match (e.g. \"5:bo2zm_cheats 1|10:bo2zm_give_weapon ray_gun_zm\"). Empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_test_all_god, false, "BO2",
                    "Test helper: ignore damage to validated local Zombies actors without changing player settings")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_first_visit_wait, false, "BO2",
                    "A map's first visit: while its graphics are prepared (FIRST VISIT - PREPARING GRAPHICS at the "
                    "top), a game this console runs for its own players only waits, its clock held almost still. "
                    "false (the default) = the game runs on and the graphics come in as they are ready");

namespace {

// The game functions the cheats call (bo2zm_guest.h); the hooked ones are with their hooks.
constexpr uint32_t kClipSize = 0x826BDC70;  // call only: a weapon's clip size
constexpr uint32_t kSetClip = 0x826BDAF8;  // call only: sets a weapon's clip
constexpr uint32_t kRadiusDamage = 0x823270B0;  // call only: radius damage
constexpr uint32_t kPlayEffect = 0x822DD668;  // call only: an effect for every local client
constexpr uint32_t kSoundAlias = 0x82584128;  // call only: a sound alias by name
constexpr uint32_t kPlaySound = 0x82230800;  // call only: plays a sound alias
constexpr uint32_t kScriptFunction = 0x8252FCB0;  // call only: a script function's code by name
constexpr uint32_t kScriptPushString = 0x8253D9C8;  // call only: pushes a string
constexpr uint32_t kScriptPushVector = 0x8253DB48;  // call only: pushes a vector
constexpr uint32_t kScriptPushInt = 0x8253D758;  // call only: pushes an int
constexpr uint32_t kScriptPushFloat = 0x8253D7B8;  // call only: pushes a float (f1)
constexpr uint32_t kScriptRunThread = 0x82543150;  // call only: runs a script function as a thread
constexpr uint32_t kScriptFreeThread = 0x8253D3F8;  // call only: lets a script thread go
constexpr uint32_t kScriptRunEntityThread = 0x8237CA30;  // call only: runs a script function on an entity
constexpr uint32_t kPerkIndex = 0x82690E50;  // call only: a perk's index by name
constexpr uint32_t kScriptGetString = 0x82543C28;  // call only: a script argument as text
constexpr uint32_t kClientFieldFind = 0x821A5BE0;  // call only: a client field by type and name
constexpr uint32_t kPlayerClientFields = 5;        // the player state's client field type
constexpr uint32_t kCameraTrace = 0x821B2AB0;  // call only: the camera trace
constexpr uint32_t kLineTrace = 0x8225CBF8;  // call only: the client's trace
constexpr uint32_t kAnglesToAxis = 0x8248E540;  // call only: angles to a view axis
constexpr uint32_t kScriptFile = 0x8252EDC0;  // call only: a loaded script's header by name
// call only: an object's field variable by name id, 0 when it has none (FindVariable). Not sub_82534918, which creates
// the variable when it is missing (sub_82534760), as "level.<field> =" needs: a scan of every object with it created
// one on each and crashed the script VM on the next server frame.
constexpr uint32_t kScriptGetVariable = 0x82534430;  // call only

constexpr uint32_t kGEntities = 0x833D0640;
constexpr uint32_t kEntClient = 340;
constexpr uint32_t kClientFlags = 24;
constexpr uint32_t kInvulnerable = 1;
constexpr uint32_t kClientThirdPerson = 132;  // a byte

constexpr uint32_t kPmPs = 0;
constexpr uint32_t kPmCmdTime = 4;
constexpr uint32_t kPmButtons = 8;  // 8 bytes
constexpr uint32_t kPmForward = 40;
constexpr uint32_t kPmRight = 41;
constexpr uint32_t kPmTraceMask = 124;
constexpr uint32_t kPsCommandTime = 0;
constexpr uint32_t kPsPmType = 4;
constexpr uint32_t kPsPmFlags = 12;
constexpr uint32_t kPmfJumping = 0x4000;
constexpr uint32_t kPsGravity = 140;  // an int
constexpr uint32_t kPsJumpOriginZ = 196;
constexpr uint32_t kPsOrigin = 40;
constexpr uint32_t kPsVelocity = 52;
constexpr uint32_t kPsViewAngles = 504;
constexpr uint32_t kPsDeltaAngles = 152;  // floats in degrees (probably: the IW engines' player state keeps them so)
constexpr uint32_t kPsClientNum = 416;
constexpr uint32_t kPsViewHeightTarget = 516;  // an int
constexpr uint32_t kPsViewHeight = 520;        // a float
constexpr uint32_t kPmNormal = 0;
constexpr uint32_t kPmSpectator = 5;

constexpr float kUfoAccel = 1400.0f;  // units/s^2 at full stick
constexpr float kUfoDrag = 2.8f;      // 1/s: top speed accel/drag = 500 units/s, eases to a stop in about a second

constexpr uint32_t kJumpHeight = 0x82085654;
constexpr float kJumpLevels[] = {39.0f, 100.0f, 250.0f, 600.0f, 1500.0f, 3500.0f};  // the game's 39, then higher
constexpr float kJumpBoost = 3000.0f;   // units/s^2: how fast a raised jump speeds up
constexpr float kJumpCruise = 3000.0f;  // units/s: the fastest rise and fall at a raised level

struct Vec3 {
  float x, y, z;
};

constexpr uint32_t kWeaponVariants = 0x845CA998;  // bg_weaponVariantDefs: 256 pointers
constexpr uint32_t kVariantName = 0;
constexpr uint32_t kVariantDef = 8;
constexpr uint32_t kDefExplosionEffect = 0x6F0;
constexpr uint32_t kDefExplosionSound = 0x71C;  // the alias name
constexpr uint32_t kTraceHit = 68;
constexpr uint32_t kModProjectileSplash = 6;  // what the game's own exploding bullets use
constexpr uint32_t kEntityWorld = 1022;
constexpr float kBlastRadius = 120.0f;
constexpr float kBlastInner = 600.0f;  // damage at the center
constexpr float kBlastOuter = 100.0f;  // at the edge
constexpr size_t kBlastsPerFrame = 3;  // explosion effects a frame at most (a shotgun's pellets)

constexpr uint32_t kScriptNoFunction = 0x829917FC;  // what the function lookup returns for a missing export
constexpr uint32_t kTlsClientFields = 6720;          // the client field registry pointer in the per-thread block
constexpr uint32_t kServerClientFields = 0x8352C5F0;  // the server's client field registry
constexpr const char* kZmScript = "maps/mp/zombies/_zm";
constexpr const char* kPowerupScript = "maps/mp/zombies/_zm_powerups";
constexpr const char* kUtilityScript = "common_scripts/utility";
constexpr float kPowerupDistance = 90.0f;  // in front of the player

// The game entity allocator's state (G_Spawn, sub_82389FB8; see the top of the file).
constexpr uint32_t kLevel = 0x83547A00;
constexpr uint32_t kLevelEntityCount = 12;  // entities handed out so far
constexpr uint32_t kLevelFreeEntity = 16;   // the first freed entity
constexpr uint32_t kEntNextFree = 780;      // the next freed entity
constexpr int kSpawnableEntities = 1022;    // G_Spawn's limit
constexpr int kSpareEntities = 128;         // a power-up spawns only with at least this many free
constexpr uint32_t kLevelRoundsPlayed = 12176;  // the round setroundsplayed stores (sub_8236C4B8)
constexpr auto kRoundChangeTimeout = std::chrono::seconds(30);  // a change the game never finished stops blocking
constexpr auto kRoundCheckEvery = std::chrono::milliseconds(250);  // a waiting round pick: checked this often

// The script variables (see the top of the file), for instance 0.
constexpr uint32_t kScrVarPub = 0x840DF080;   // 84 bytes per instance
constexpr uint32_t kScrLevelObject = 0x1C;    // the level object's id
constexpr uint32_t kScrVarGlob = 0x840DEC80;  // 512 bytes per instance
constexpr uint32_t kScrObjects = 0;           // 8 bytes per object: its type in the low 7 bits of + 4
constexpr uint32_t kScrValues = 0x180;        // 16 bytes per variable: the value at + 0, its type byte at + 6
constexpr uint32_t kScrFieldObjectTypes = 0x12;  // object types up to this hold named fields
constexpr uint8_t kScrInt = 7;
constexpr uint8_t kScrString = 2;  // a string variable: the value is its string id (sub_8253D9C8 pushes type 2)
constexpr uint8_t kFixupFieldName = 1;        // a string fixup's type for a field name
// The server instance's parent variables (objects, arrays, threads...), 8 bytes each from the word at kScrVarGlob +
// kScrObjects: sub_825341E8 makes 24576 for the server (16384 for the client). The free ones are a list from entry 0
// through the halfwords at + 2 (+ 0 links back); sub_82534EC0, which hands them out, raises "exceeded maximum number
// of parent %s script variables" (0x82069030) when it is empty.
constexpr uint32_t kServerParentVariables = 24576;
constexpr auto kParentsReportEvery = std::chrono::seconds(60);

constexpr uint32_t kEntitySize = 796;
constexpr uint32_t kEntityCount = 1024;
constexpr uint32_t kEntActor = 344;
constexpr uint32_t kEntSentient = 348;
constexpr uint32_t kEntOrigin = 308;
constexpr uint32_t kEntHealth = 424;
constexpr uint32_t kSentientIgnoreMe = 0x14;  // a byte
constexpr float kKillDamage = 1000000.0f;

constexpr uint32_t kClientSessionState = 21520;
constexpr uint32_t kClientWeapon = 440;  // a word; the low byte is the weapon index
constexpr uint32_t kClientScore = 0x55C8;
constexpr uint32_t kMaxPoints = 1000000;

constexpr uint32_t kCgArray = 0x82BBAE68;
constexpr uint32_t kCgSize = 0x87700;
constexpr uint32_t kCgViewOrigin = 0x4D8B8;
constexpr uint32_t kThirdPersonRangeDvar = 0x82BBC584;
constexpr uint32_t kDvarCurrent = 0x18;
constexpr float kShoulderRight = 26.0f;  // units to the right of the eye while aiming
constexpr float kShoulderUp = -6.0f;  // a little below where the game puts the camera (kCameraLift above the head)
constexpr float kShoulderRange = 55.0f;  // camera distance while aiming
constexpr float kShoulderEase = 8.0f;    // 1/s: about an eighth of a second in and out
constexpr uint32_t kCgViewAxis = 0x4D8C8;  // 3 x 3 floats
constexpr uint32_t kThirdPersonAngleDvar = 0x82BC27A8;  // cg_thirdPersonAngle's dvar pointer
constexpr int kCrosshairColors = 7;  // the output pass's (bo2zm_output_gamma_ramp.frag)
constexpr uint32_t kShotMask = 0x0280E833;  // the bullets' trace mask (as the bullet trace uses it)
constexpr uint32_t kCameraMask = 0x00810811;  // the third person camera's
constexpr uint32_t kCgBaseAngles = 0x64920;   // the view angles the third person camera starts from
constexpr float kCameraLift = 8.0f;           // the camera starts this far above the head
constexpr uint32_t kCgPassEntity = 0x481F4;   // the entity the camera trace passes
constexpr uint32_t kCgPassEntityOther = 0x48248;
constexpr uint32_t kEntityNone = 1023;
constexpr uint32_t kThirdPersonModeDvar = 0x82B25F64;  // cg_thirdPersonMode's dvar pointer
constexpr float kOrbitPitchScale = 0.5f;      // the camera backs off along half the view pitch
constexpr float kFocusDistance = 512.0f;      // the game's camera looks at this point ahead of the head
constexpr float kMinAimDistance = 48.0f;
constexpr float kMaxTurn = 20.0f;             // degrees from the game's camera direction
constexpr float kAimReach = 8192.0f;

constexpr int kFovDegrees[] = {55, 60, 65, 70, 75, 80, 85, 90, 95, 100, 105, 110};
constexpr int kFovNormal = 2;  // 65, the game's

constexpr float kFeatherFall = 120.0f;  // units/s: the fastest fall with feather fall
constexpr float kGlideAccel = 900.0f;   // units/s^2 of steering in the air
constexpr float kGlideSpeed = 400.0f;   // units/s: the fastest glide

constexpr int kCheckEveryFrames = 15;  // held dvars: about four times a second

using bo2zm::cheats::detail::Option;
using bo2zm::cheats::detail::PlayerOptions;
constexpr int kLocalPlayers = bo2zm::cheats::detail::kLocalPlayers;
// Players of other consoles in a System Link game this console hosts (room for eight, Grief's most).
constexpr int kRemotePlayers = 8;

struct PlayerState {
  PlayerOptions options;
  std::atomic<uint32_t> god_blocked{0};
  std::atomic<bool> aiming{false};
  std::atomic<bool> in_match{false};
  uint64_t main_epoch = 0, server_epoch = 0, camera_epoch = 0;
  uint64_t main_generation = 0, server_generation = 0, camera_generation = 0;
  // Main-thread caches, owned by this local player.
  bool god_was_on = false, third_was_on = false, ignore_was_on = false;
  uint32_t sized_weapon = 0, left_weapon = 0;
  int32_t clip_size = 0, left_size = 0;
  bool reported_god = false;
  int32_t last_health = 0;
  int health_falls_logged = 0;
  std::chrono::steady_clock::time_point next_god_report{};
  // Camera-hook caches, keyed by the engine's explicit local-client argument.
  float shoulder_blend = 0.0f;
  std::chrono::steady_clock::time_point shoulder_last{};
  bool aim_logged = false, shift_logged = false;
  int aims_logged = 0;
  // Server-thread perk history; never shared with another local player.
  uint32_t noted_perks[2] = {};
  int32_t speed_written = INT32_MIN;  // Server thread: the move speed this program last wrote (ScaleMoveSpeed)
  std::chrono::steady_clock::time_point perks_gone_since{}, perks_quiet_until{};
  bool perks_was_down = false, perks_active = false;
};
std::array<PlayerState, kLocalPlayers> g_players;
std::array<PlayerState, kRemotePlayers> g_remote_players;

// A copied actor identity, never an implicit current-player scope. Queued work
// carries this identity and is discarded when its local view changes actor.
// A remote actor (a player of another console in a System Link game this console hosts) has no local view: local
// and controller are -1, cg is 0, remote is its slot in g_remote_players and xuid the XUID it was matched by.
// A predicted actor is the other side of it: a local view on a console that joined someone else's System Link game.
// It has its view (cg, its predicted player state at cg + 0x480A8) and no server entity or client here (both 0), so
// only the client's side of the cheats acts on it: the prediction's movement step, the camera, the field of view, the
// view kick and the crosshair (see "Remote players" below).
struct Actor {
  int local = -1, controller = -1;
  uint32_t number = 0, entity = 0, client = 0, cg = 0;
  uint64_t generation = 0;
  uint64_t epoch = 0;
  int remote = -1;
  uint64_t xuid = 0;
  bool predicted = false;
  explicit operator bool() const { return (local >= 0 || remote >= 0) && (client != 0 || predicted); }
};
std::mutex g_actors_mutex;
std::array<Actor, kLocalPlayers> g_actors;
// Remote players (System Link; see "Remote players" further down). Their options come from the network, and each
// slot's state lasts as long as its player plays here.
std::array<Actor, kRemotePlayers> g_remote_actors;  // g_actors_mutex
std::atomic<int> g_remote_count{0};  // remote actors mapped now (a fast way past the remote lookups)
std::atomic<bool> g_forget_remote{false};  // a map load: the main frame forgets every remote player
std::atomic<bool> g_joined{false};  // this console is in someone else's System Link match (UpdateActors)
uint64_t g_actor_revision = 0;  // global across all local slots; g_actors_mutex
std::array<int, kLocalPlayers> g_controllers{-1, -1, -1, -1};
std::atomic<int> g_primary_local{0};
std::atomic<int> g_primary_controller{0};
std::atomic<bool> g_legacy_boot_picks{false};
std::atomic<bool> g_primary_bound{false};
std::atomic<uint64_t> g_map_epoch{1};
std::atomic<int> g_active_locals{0};
std::mutex g_crosshair_mutex;
std::array<bo2zm::cheats::CrosshairOverlay, kLocalPlayers> g_crosshair_overlays;
int g_crosshair_count = 0;
std::atomic<bool> g_map_loading{false};
Actor LocalActor(int local) {
  if (local < 0 || local >= kLocalPlayers) return {};
  std::lock_guard lock(g_actors_mutex);
  return g_actors[local];
}
PlayerState& StateForLocal(int local) {
  std::lock_guard lock(g_actors_mutex);
  const int controller = local >= 0 && local < kLocalPlayers ? g_controllers[local] : -1;
  return g_players[controller >= 0 ? controller : local >= 0 && local < kLocalPlayers ? local : 0];
}
PlayerState& StateForActor(const Actor& actor) {
  return actor.remote >= 0 ? g_remote_players[actor.remote] : g_players[actor.controller];
}
Actor RemoteActor(int remote) {
  if (remote < 0 || remote >= kRemotePlayers) return {};
  std::lock_guard lock(g_actors_mutex);
  return g_remote_actors[remote];
}
// For log lines: "local player N" or "remote player <XUID> (client N)".
std::string ActorName(const Actor& actor) {
  if (actor.remote >= 0) return fmt::format("remote player {:016X} (client {})", actor.xuid, actor.number);
  return fmt::format("local player {}", actor.local + 1);
}
PlayerState& PrimaryState() { return g_players[g_primary_controller.load(std::memory_order_relaxed)]; }
Actor PrimaryActor() { return LocalActor(g_primary_local.load(std::memory_order_relaxed)); }
bool AnyOption(Option option) {
  for (const auto& state : g_players) if (state.options.Effective(option)) return true;
  for (const auto& state : g_remote_players) if (state.options.Effective(option)) return true;
  return false;
}
void LegacyOption(Option option, int value) {
  if (!g_primary_bound) g_legacy_boot_picks.store(true, std::memory_order_relaxed);
  PrimaryState().options.Direct(option, value);
}

std::atomic<bool> g_stop_spawns{false};
std::atomic<bool> g_kill_zombies{false};
std::mutex g_script_mutex;
Actor g_kill_target;  // under g_script_mutex
enum class PlayerAction { Perk, RemovePerks, Gun, Pap, Points, Powerup };
struct PlayerRequest {
  Actor actor;
  PlayerAction action;
  std::string argument;
  bool pap = false;
};
std::vector<PlayerRequest> g_player_requests;  // g_script_mutex; targets copied before queueing
std::atomic<bool> g_player_waiting{false};
int g_round_request = 0;                      // a round to go to, 0 = none; under g_script_mutex
Actor g_pending_round_target;  // Server thread
int g_pending_round = 0;                      // Server thread: the round to go to once a round is on
std::atomic<bool> g_script_waiting{false};    // a power-up, perk or round request is waiting
int g_doors_request = 0;                      // 1 open all doors, 2 close them; under g_script_mutex
int g_power_request = 0;                      // 1 power on, 2 off; under g_script_mutex
int g_debris_request = 0;                     // 1 remove free debris, 2 put it back; under g_script_mutex
Actor g_world_target;  // under g_script_mutex
Actor g_round_target;  // under g_script_mutex; Server thread keeps its captured copy
std::atomic<bool> g_world_waiting{false};     // a gun, Pack-a-Punch, door or power request is waiting
// Keep perks' notes (Server thread only): the perk bits seen while the selected player was up, since when a noted perk has been
// gone while they stayed up (zero: none), and until when nothing is noted after REMOVE PERKS.
constexpr auto kForgetPerkTime = std::chrono::milliseconds(500);  // longer than going down takes to show

constexpr const char* kPerksScript = "maps/mp/zombies/_zm_perks";
constexpr uint32_t kPsPerks = 1352;  // two words of perk bits
constexpr uint32_t kPmNormalLinked = 1;
// The zombies perks (the machines' perk names), for keep perks.
constexpr const char* kZombiePerks[] = {
    "specialty_armorvest",      "specialty_quickrevive",    "specialty_fastreload",
    "specialty_rof",            "specialty_longersprint",   "specialty_flakjacket",
    "specialty_deadshot",       "specialty_additionalprimaryweapon", "specialty_scavenger",
    "specialty_finalstand",     "specialty_grenadepulldeath", "specialty_nomotionsensor"};
std::mutex g_blasts_mutex;
struct Blast { Actor actor; Vec3 at; };
std::vector<Blast> g_blasts;  // the selected player's bullet hits that exploded, for the client's effect
struct RemoteBlast { uint64_t xuid; Vec3 at; };
std::vector<RemoteBlast> g_remote_blasts;  // remote players' hits, for the consoles they play on (g_blasts_mutex)
std::atomic<bool> g_blasts_waiting{false};
std::atomic<bool> g_in_match{false};
std::atomic<int> g_round_shown{0};  // the game's round counter as of the last frame (CurrentRound)
std::atomic<int> g_parents_in_use{-1};  // the server's parent script variables in use, about once a second

// A dvar the menu asked to hold. Main thread only, but the console hook and the frame both touch the list.
struct Held {
  std::string name;
  std::string value;     // what to hold it at
  std::string original;  // the game's value when it was first held ("" = not seen yet)
  bool release = false;  // put the original back and forget it
  int fixes = 0;         // times the game changed it back (logged a few times)
  bool menu = false;     // held by the menu's bo2zm_hold (CHEATS off parks it)
  bool mirror = false;   // a joined match's world setting, as its host holds it (ApplyWorldState)
  std::optional<bo2zm::dvars::Dvar> dvar;  // once registered (a dvar's place never changes)
};
std::mutex g_held_mutex;
std::vector<Held> g_held;
std::vector<std::pair<std::string, std::string>> g_parked;  // the menu's held dvars while CHEATS is off: name, value
// The menu's picks to show: bo2zm_hold_<dvar> = the picked value (the menu reads it; created here, on the main frame,
// so the menu script never runs a console set itself).
std::vector<std::pair<std::string, std::string>> g_shown;
std::atomic<bool> g_held_dirty{false};
std::atomic<bool> g_held_any{false};  // g_held is not empty (read without the lock)

// Holds a dvar for the program itself (no menu dvar), or gives it back. Caller holds g_held_mutex.
void HoldLocked(const std::string& dvar, const std::string& value, bool hold) {
  Held* entry = nullptr;
  for (Held& h : g_held) {
    if (h.name == dvar) {
      entry = &h;
    }
  }
  if (!entry) {
    if (!hold) {
      return;
    }
    entry = &g_held.emplace_back();
    entry->name = dvar;
  }
  entry->value = value;
  entry->release = !hold;
  entry->fixes = 0;
  g_held_dirty.store(true, std::memory_order_relaxed);
  g_held_any.store(true, std::memory_order_relaxed);
}

void Show(const std::string& dvar, const std::string& value) {
  for (auto& [name, shown] : g_shown) {
    if (name == dvar) {
      shown = value;
      return;
    }
  }
  g_shown.emplace_back(dvar, value);
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

uint16_t Load16(const uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, base + address, 2);
  return __builtin_bswap16(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(base, address, bits);
}

Vec3 LoadVec(const uint8_t* base, uint32_t address) {
  return {LoadFloat(base, address), LoadFloat(base, address + 4), LoadFloat(base, address + 8)};
}

void StoreVec(uint8_t* base, uint32_t address, const Vec3& v) {
  StoreFloat(base, address, v.x);
  StoreFloat(base, address + 4, v.y);
  StoreFloat(base, address + 8, v.z);
}

float Dot(const Vec3& a, const Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

const char* FlyName(int mode) {
  return mode == bo2zm::cheats::kNoclip ? "noclip" : mode == bo2zm::cheats::kUfo ? "ufo" : "off";
}

// The primary actor's client, or zero outside a live owned match.
bool CurrentActor(const uint8_t* base, const Actor& actor);
uint32_t Client(const uint8_t* base) {
  const Actor actor = PrimaryActor();
  return CurrentActor(base, actor) ? actor.client : 0;
}

bool SameActor(const Actor& a, const Actor& b) {
  return a.local == b.local && a.controller == b.controller && a.number == b.number &&
         a.entity == b.entity && a.client == b.client && a.cg == b.cg && a.generation == b.generation &&
         a.epoch == b.epoch && a.remote == b.remote && a.xuid == b.xuid && a.predicted == b.predicted;
}
bool GuestRange(uint32_t pointer, uint32_t size) {
  return pointer >= 0x40000000u && uint64_t(pointer) + size <= 0x100000000ull;
}
bool CurrentRemoteActor(const uint8_t* base, const Actor& actor);
bool CurrentActor(const uint8_t* base, const Actor& actor) {
  if (actor.remote >= 0) return CurrentRemoteActor(base, actor);
  if (!actor || actor.local >= kLocalPlayers || actor.controller < 0 || actor.controller >= kLocalPlayers ||
      g_map_loading.load(std::memory_order_acquire) || actor.epoch != g_map_epoch.load(std::memory_order_acquire) ||
      !SameActor(actor, LocalActor(actor.local)) || (!actor.predicted && !GuestRange(actor.client, 22520)) ||
      !GuestRange(actor.cg, kCgSize)) return false;
  const uint32_t allocated = Load32(base, 0x82C70F44);
  const uint64_t cg = uint64_t(Load32(base, kCgArray)) + uint64_t(actor.local) * kCgSize;
  const uint32_t manager = 0x837F6A28 + uint32_t(actor.local) * 20;
  // Validate live ownership too: a leave/swap can precede the next main-frame
  // publication, including while a server request batch is already drained.
  const bool view = allocated <= kLocalPlayers && uint32_t(actor.local) < allocated && cg == actor.cg &&
                    (Load32(base, manager) & 1) && Load32(base, manager + 4) == uint32_t(actor.local) &&
                    Load32(base, manager + 8) == uint32_t(actor.controller) &&
                    base[0x83E402C0 + uint32_t(actor.controller) * 172] &&
                    (Load32(base, 0x82C6FDC8 + uint32_t(actor.local) * 1112) & 1) &&
                    Load32(base, actor.cg) == actor.number;
  if (actor.predicted) {
    return view && actor.entity == 0 && actor.client == 0 && g_joined.load(std::memory_order_acquire);
  }
  return view && base[actor.entity + 251] && Load32(base, actor.entity + kEntClient) == actor.client &&
         Load32(base, actor.client + kPsClientNum) == actor.number;
}

// This console's own server runs: sv_running (we take it as the IW engines' running-server flag) and a client on
// the first player entity (the preset's test). A console that joined another's System Link game has neither.
bool LocalServerRunning(const uint8_t* base) {
  static std::optional<bo2zm::dvars::Dvar> running;
  if (!running) running = bo2zm::dvars::Find("sv_running");
  return (!running || base[running->pointer + kDvarCurrent] != 0) &&
         Load32(base, kGEntities + kEntClient) >= 0x82000000u;
}

// Numeric identity helpers 82403F58/82403BC8/82403CF8 use these four 20-byte
// manager records. 824DF918 reads the controller-used byte, and 827503A8 reads
// the separate runtime-active bit. CG allocation/count is proved by 821DF9B8.
void UpdateActors(const uint8_t* base) {
  const bool joined = bo2zm::mode::IsZombies() && bo2zm::preset::ClientInGame() && !LocalServerRunning(base);
  if (joined != g_joined.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[cheats] {}", joined ? "in a match another console hosts: the local players' cheats act on their "
                                        "prediction here and are sent to the host"
                                      : "no longer in a match another console hosts");
  }
  g_joined.store(joined, std::memory_order_release);
  constexpr uint32_t manager = 0x837F6A28;
  constexpr uint32_t controller_used = 0x83E402C0;
  constexpr uint32_t runtime_active = 0x82C6FDC8;
  const uint64_t epoch = g_map_epoch.load(std::memory_order_acquire);
  const uint32_t count = Load32(base, 0x82C70F44);
  const uint32_t allocated = count <= kLocalPlayers ? count : 0;
  const uint32_t cg_array = Load32(base, kCgArray);
  std::array<Actor, kLocalPlayers> next{};
  std::array<int, kLocalPlayers> controllers{-1, -1, -1, -1};
  for (int local = 0; local < kLocalPlayers; ++local) {
    const uint32_t record = manager + uint32_t(local) * 20;
    const uint32_t controller = Load32(base, record + 8);
    if (Load32(base, record + 4) != uint32_t(local) || controller >= kLocalPlayers ||
        !(Load32(base, record) & 1) || !base[controller_used + controller * 172]) continue;
    controllers[local] = int(controller);
    if (g_map_loading.load(std::memory_order_acquire)) continue;
    if (uint32_t(local) >= allocated || !GuestRange(cg_array, allocated * kCgSize) ||
        !(Load32(base, runtime_active + uint32_t(local) * 1112) & 1)) continue;
    const uint64_t cg_wide = uint64_t(cg_array) + uint64_t(local) * kCgSize;
    if (cg_wide + kCgSize > 0x100000000ull) continue;
    const uint32_t cg = uint32_t(cg_wide);
    // Engine.GetClientNum (82728610) reads cg+0. It is a server client number,
    // not a local view/controller index. The authoritative PS starts at client.
    const uint32_t number = Load32(base, cg);
    if (number >= kSpawnableEntities) continue;
    if (joined) {
      next[local] = {local, int(controller), number, 0, 0, cg, 0, epoch};
      next[local].predicted = true;
      continue;
    }
    const uint32_t entity = kGEntities + number * kEntitySize;
    const uint32_t client = Load32(base, entity + kEntClient);
    if (!GuestRange(client, 22520) || !base[entity + 251] || Load32(base, client + kPsClientNum) != number) continue;
    next[local] = {local, int(controller), number, entity, client, cg, 0, epoch};
  }
  const int primary = int32_t(Load32(base, 0x82976AC8));  // 82403C88 / 82404078
  if (!g_primary_bound && primary >= 0 && primary < kLocalPlayers && controllers[primary] >= 0) {
    const int controller = controllers[primary];
    if (controller != 0 && g_legacy_boot_picks.load(std::memory_order_relaxed)) {
      g_players[controller].options.CopyFrom(g_players[0].options);
      g_players[0].options.ResetAll();
    }
    g_primary_bound = true;
  }
  {
    std::lock_guard lock(g_actors_mutex);
    if (epoch != g_map_epoch.load(std::memory_order_acquire) || g_map_loading.load(std::memory_order_acquire)) next = {};
    for (int local = 0; local < kLocalPlayers; ++local) {
      auto& actor = next[local];
      const auto& previous = g_actors[local];
      actor.generation = previous.generation;
      if (actor.local != previous.local || actor.controller != previous.controller || actor.number != previous.number ||
          actor.entity != previous.entity || actor.client != previous.client || actor.cg != previous.cg ||
          actor.predicted != previous.predicted) {
        actor.generation = ++g_actor_revision;
      }
    }
    g_actors = next;
    g_controllers = controllers;
    if (primary >= 0 && primary < kLocalPlayers && controllers[primary] >= 0)
      g_primary_controller.store(controllers[primary], std::memory_order_relaxed);
    g_primary_local.store(primary >= 0 && primary < kLocalPlayers && controllers[primary] >= 0 ? primary : -1,
                          std::memory_order_relaxed);
  }
  std::array<bool, kLocalPlayers> present{};
  int active = 0;
  for (const auto& actor : next) {
    if (actor) {
      present[actor.controller] = true;
      ++active;
    }
  }
  for (int controller = 0; controller < kLocalPlayers; ++controller) {
    auto& state = g_players[controller];
    state.in_match.store(present[controller], std::memory_order_relaxed);
    if (!present[controller]) {
      state.aiming.store(false, std::memory_order_relaxed);
      state.options.CancelCombo();
    }
  }
  g_active_locals.store(active, std::memory_order_relaxed);
  // A leave/reorder can invalidate one player while the other views continue.
  // Do not leave that player's old requests parked until a later server frame.
  std::array<Actor, kRemotePlayers> remotes;
  {
    std::lock_guard lock(g_actors_mutex);
    remotes = g_remote_actors;
  }
  {
    std::lock_guard lock(g_script_mutex);
    const auto matches = [&](const Actor& actor) {
      if (actor.remote >= 0) {
        return actor.remote < kRemotePlayers && remotes[actor.remote] && SameActor(actor, remotes[actor.remote]);
      }
      return actor.local >= 0 && actor.local < kLocalPlayers && next[actor.local] &&
             SameActor(actor, next[actor.local]);
    };
    std::erase_if(g_player_requests, [&](const auto& request) { return !matches(request.actor); });
    g_player_waiting.store(!g_player_requests.empty(), std::memory_order_release);
    if (!matches(g_world_target)) {
      g_doors_request = g_power_request = g_debris_request = 0;
      g_world_waiting.store(false, std::memory_order_relaxed);
    }
    if (!matches(g_round_target)) {
      g_round_request = 0;
      g_script_waiting.store(false, std::memory_order_relaxed);
    }
  }
}

Actor ActorForEntity(const uint8_t* base, uint32_t entity) {
  for (int local = 0; local < kLocalPlayers; ++local) {
    const Actor actor = LocalActor(local);
    if (actor && actor.entity && actor.entity == entity && CurrentActor(base, actor)) return actor;
  }
  if (g_remote_count.load(std::memory_order_relaxed) == 0) [[likely]] return {};
  for (int remote = 0; remote < kRemotePlayers; ++remote) {
    const Actor actor = RemoteActor(remote);
    if (actor && actor.entity == entity && CurrentActor(base, actor)) return actor;
  }
  return {};
}
// A remote actor has only the server's player state (the console it plays on predicts its own copy).
Actor ActorForPs(const uint8_t* base, uint32_t ps) {
  if (ps < 0x10000) return {};
  for (int local = 0; local < kLocalPlayers; ++local) {
    const Actor actor = LocalActor(local);
    if (actor && (ps == actor.client || ps == actor.cg + 0x480A8) &&
        Load32(base, ps + kPsClientNum) == actor.number && CurrentActor(base, actor)) return actor;
  }
  if (g_remote_count.load(std::memory_order_relaxed) == 0) [[likely]] return {};
  for (int remote = 0; remote < kRemotePlayers; ++remote) {
    const Actor actor = RemoteActor(remote);
    if (actor && ps == actor.client && Load32(base, ps + kPsClientNum) == actor.number && CurrentActor(base, actor))
      return actor;
  }
  return {};
}
Actor ActorForPm(const uint8_t* base, uint32_t pm) {
  if (!GuestRange(pm, 612)) return {};
  const uint32_t ps = Load32(base, pm + kPmPs);
  const Actor actor = ActorForPs(base, ps);
  if (!actor) return {};
  // Prediction publishes its local at pm+608 and has domain byte 0 at +604;
  // server movement publishes -1/domain 1 (821FF9A8, 8230E4A4-8230E508).
  const int local = int32_t(Load32(base, pm + 608));
  const bool server = base[pm + 604] != 0;
  if (actor.remote >= 0) return server && local == -1 && ps == actor.client ? actor : Actor{};
  return (server && local == -1 && ps == actor.client) ||
                 (!server && local == actor.local && ps == actor.cg + 0x480A8) ? actor : Actor{};
}

// ---- Remote players (System Link) ----
//
// A console that joined another's System Link game runs no server of its own, so the server's side of its cheats has
// nothing to act on there. Each cheat runs where its half of the game runs:
//   - On the joining console, its local views become predicted actors (UpdateActors, while JoinedRemoteGame): the
//     client's side of the cheats resolves them as it resolves a local actor, on the predicted player state at cg +
//     0x480A8, so the prediction's movement step flies, glides and jumps the way the host's server moves the player,
//     and the view kick, the field of view, the third person camera and crosshair and the fly combo work as they do
//     for a local player. What needs the server (god mode, ammo, zombies ignore, perks, guns, points, power-ups) and
//     the match-wide commands (rounds, doors, power, debris, stop spawns, kill all, the world settings) go to the host
//     (bo2zm_cheat_sync.cpp: the options with SetRemotePlayer, the commands with RunRemoteCommand).
//   - On the host, each remote player gets a PlayerState of its own in g_remote_players and an Actor with remote set,
//     which the hooks (god mode's damage hook, the movement step, move speed, the jump, recoil and bullet hooks) and
//     the frame's per-player work resolve like a local actor's.
// The world settings (game speed, zombies freeze, gravity, unlimited sprint) are engine dvars the whole match shares.
// The host holds them, and a joining console holds the host's values too (ApplyWorldState): the prediction reads
// gravity and sprint itself, and the menu shows the host's picks.
//
// The player is found by XUID, the way the script method getxuid (sub_8231DC68, method table entry at 0x820253C0)
// does it: the entity's client (entity + 340) holds at + 21744 its slot in the server's client array, and
// sub_8242CD60 returns the 64-bit XUID of slot i from 0x83B5D630 + i * 8. That array lives in the server's static
// state at 0x83B50880 (+ 0xCDB0). The connect handler (sub_8242E818, probably SV_DirectConnect: it reads the connect
// string's "challenge", "protocol" and "xuid" and answers "connectResponse") compares a new client's "xuid" with
// every connected client's entry and clears the new client's; the userinfo handler (sub_8242DE38, probably
// SV_UserinfoChanged) parses the "xuid" key and copies it into the client's entry (the memcpy after
// loc_8242E0D8); the timeout check (sub_824355E8) walks the same array beside the client array. Player entities are
// the first entities, numbered like their clients (ps->clientNum at client + 416 is the entity number, which
// UpdateActors already relies on).
//
// The explosive bullets' explosions are drawn by the client: the host sends a remote player's hits back to the console
// it plays on (TakeRemoteBlasts, PlayRemoteBlast).
constexpr uint32_t kServerXuids = 0x83B5D630;  // the server's XUID per client slot (u64 each)
constexpr uint32_t kClientXuidSlot = 21744;    // the client's slot in it (a word)
// The player entities looked at: the IW engines' MAX_CLIENTS is 18, and we believe BO2's is too (Zombies uses at
// most eight).
constexpr uint32_t kMaxServerClients = 18;
constexpr auto kRemoteLeftAfter = std::chrono::seconds(5);       // mapped once, then gone this long: it left
constexpr auto kRemoteNeverMapped = std::chrono::seconds(30);    // never found among the server's players

uint64_t Load64(const uint8_t* base, uint32_t address) {
  uint64_t v;
  std::memcpy(&v, base + address, 8);
  return __builtin_bswap64(v);
}

// The XUID the server keeps for this client (what getxuid returns), 0 when the slot is out of range.
uint64_t ServerXuid(const uint8_t* base, uint32_t client) {
  const uint32_t slot = Load32(base, client + kClientXuidSlot);
  return slot < kMaxServerClients ? Load64(base, kServerXuids + slot * 8) : 0;
}

// A player entity of this console's server: its number, entity and client. false when entity number is not one.
bool ServerPlayer(const uint8_t* base, uint32_t number, uint32_t& entity, uint32_t& client) {
  entity = kGEntities + number * kEntitySize;
  client = Load32(base, entity + kEntClient);
  return base[entity + 251] && GuestRange(client, 22520) && Load32(base, client + kPsClientNum) == number;
}

bool CurrentRemoteActor(const uint8_t* base, const Actor& actor) {
  if (!actor || actor.remote >= kRemotePlayers || actor.number >= kMaxServerClients || !actor.xuid ||
      g_map_loading.load(std::memory_order_acquire) || actor.epoch != g_map_epoch.load(std::memory_order_acquire) ||
      !SameActor(actor, RemoteActor(actor.remote))) return false;
  uint32_t entity = 0, client = 0;
  return ServerPlayer(base, actor.number, entity, client) && entity == actor.entity && client == actor.client &&
         ServerXuid(base, client) == actor.xuid;
}

// Main thread only (the receiver and the frame both run there).
struct RemoteSlot {
  uint64_t xuid = 0;  // 0: free
  uint32_t sequence = 0;
  int client = -1;  // the server client it maps to, -1 none
  bool mapped_once = false;
  std::chrono::steady_clock::time_point since{};  // created, or last lost its client
};
std::array<RemoteSlot, kRemotePlayers> g_remote_slots;

// The server client playing with this XUID that is not one of this console's own players, or -1.
int FindRemoteClient(const uint8_t* base, uint64_t xuid, uint32_t& entity, uint32_t& client) {
  if (!xuid || g_map_loading.load(std::memory_order_acquire)) return -1;
  std::array<bool, kMaxServerClients> local{};
  {
    std::lock_guard lock(g_actors_mutex);
    for (const auto& actor : g_actors) {
      if (actor && actor.number < kMaxServerClients) local[actor.number] = true;
    }
  }
  for (uint32_t number = 0; number < kMaxServerClients; ++number) {
    if (local[number] || !ServerPlayer(base, number, entity, client)) continue;
    if (ServerXuid(base, client) == xuid) return int(number);
  }
  return -1;
}

void ForgetRemote(int remote, const char* why) {
  auto& slot = g_remote_slots[remote];
  if (!slot.xuid) return;
  REXLOG_INFO("[cheat sync] remote player {:016X}: forgotten ({})", slot.xuid, why);
  slot = {};
  g_remote_players[remote].options.ResetAll();
  g_remote_players[remote].in_match.store(false, std::memory_order_relaxed);
}

// The remote actors, from the remote slots and this frame's server state. Main thread, every frame.
void UpdateRemoteActors(const uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  if (g_forget_remote.exchange(false, std::memory_order_acq_rel)) {
    for (int remote = 0; remote < kRemotePlayers; ++remote) ForgetRemote(remote, "map load");
  }
  bool any = false;
  for (const auto& slot : g_remote_slots) any |= slot.xuid != 0;
  if (!any) {
    if (g_remote_count.load(std::memory_order_relaxed) != 0) {
      std::lock_guard lock(g_actors_mutex);
      g_remote_actors = {};
      g_remote_count.store(0, std::memory_order_relaxed);
    }
    return;
  }
  const uint64_t epoch = g_map_epoch.load(std::memory_order_acquire);
  const auto now = Clock::now();
  std::array<Actor, kRemotePlayers> next{};
  for (int remote = 0; remote < kRemotePlayers; ++remote) {
    auto& slot = g_remote_slots[remote];
    if (!slot.xuid) continue;
    uint32_t entity = 0, client = 0;
    const int number = FindRemoteClient(base, slot.xuid, entity, client);
    if (number != slot.client) {
      if (number >= 0) {
        REXLOG_INFO("[cheat sync] remote player {:016X} is server client {} (entity {:08X}, client {:08X})",
                    slot.xuid, number, entity, client);
      } else {
        REXLOG_INFO("[cheat sync] remote player {:016X} is no longer among the server's players (was client {})",
                    slot.xuid, slot.client);
      }
      slot.client = number;
      slot.since = now;
    }
    if (number >= 0) {
      slot.mapped_once = true;
      next[remote] = {-1, -1, uint32_t(number), entity, client, 0, 0, epoch, remote, slot.xuid};
    } else if (now - slot.since >= (slot.mapped_once ? kRemoteLeftAfter : kRemoteNeverMapped)) {
      ForgetRemote(remote, slot.mapped_once ? "left the game" : "never joined this console's game");
    }
  }
  int count = 0;
  {
    std::lock_guard lock(g_actors_mutex);
    if (epoch != g_map_epoch.load(std::memory_order_acquire) || g_map_loading.load(std::memory_order_acquire)) next = {};
    for (int remote = 0; remote < kRemotePlayers; ++remote) {
      auto& actor = next[remote];
      const auto& previous = g_remote_actors[remote];
      actor.generation = previous.generation;
      if (actor.number != previous.number || actor.entity != previous.entity || actor.client != previous.client ||
          actor.xuid != previous.xuid || bool(actor) != bool(previous)) {
        actor.generation = ++g_actor_revision;
      }
      count += bool(actor);
    }
    g_remote_actors = next;
    g_remote_count.store(count, std::memory_order_relaxed);
  }
  for (int remote = 0; remote < kRemotePlayers; ++remote) {
    g_remote_players[remote].in_match.store(bool(next[remote]), std::memory_order_relaxed);
  }
}

void PublishCrosshairOverlays(const uint8_t* base) {
  std::array<bo2zm::cheats::CrosshairOverlay, kLocalPlayers> next{};
  int count = 0;
  const uint64_t epoch = g_map_epoch.load(std::memory_order_acquire);
  // 82261D30 returns these complete output dimensions. 8223CAF8 uses them
  // to turn normalized layout into pixels before 8223BD90 copies the CG rect.
  const int width = int32_t(Load32(base, 0x82C153E8));
  const int height = int32_t(Load32(base, 0x82C153EC));
  if (bo2zm::mode::IsZombies() && width > 0 && width <= 16384 && height > 0 && height <= 16384) {
    for (int local = 0; local < kLocalPlayers; ++local) {
      const Actor actor = LocalActor(local);
      if (!CurrentActor(base, actor)) continue;
      const int x = int32_t(Load32(base, actor.cg + 0x4D880));
      const int y = int32_t(Load32(base, actor.cg + 0x4D884));
      const int w = int32_t(Load32(base, actor.cg + 0x4D888));
      const int h = int32_t(Load32(base, actor.cg + 0x4D88C));
      if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > width || h > height || x > width - w || y > height - h) continue;
      const auto& state = StateForActor(actor);
      const int crosshair = state.options.Effective(Option::ThirdCrosshair);
      const bool enabled = state.options.Effective(Option::Third) &&
                           (crosshair == 2 || (crosshair == 1 && state.aiming.load(std::memory_order_relaxed)));
      next[count++] = {{float(x) / width, float(y) / height, float(w) / width, float(h) / height}, enabled,
                      state.options.Effective(Option::CrosshairType), state.options.Effective(Option::CrosshairColor)};
    }
  }
  std::lock_guard lock(g_crosshair_mutex);
  if (epoch != g_map_epoch.load(std::memory_order_acquire) || g_map_loading.load(std::memory_order_acquire)) count = 0;
  g_crosshair_overlays = next;
  g_crosshair_count = count;
}

// Keeps a bit of a client word set (or clear) while a cheat is on, and puts it back once when it goes off.
void KeepBit(uint8_t* base, uint32_t client, uint32_t offset, uint32_t bit, bool on, bool set_while_on, bool& was_on) {
  if (!on && !was_on) [[likely]] {
    return;
  }
  if (!client) {
    return;
  }
  const uint32_t word = Load32(base, client + offset);
  const bool want = on == set_while_on;
  if (want && !(word & bit)) {
    Store32(base, client + offset, word | bit);
  } else if (!want && (word & bit)) {
    Store32(base, client + offset, word & ~bit);
  }
  was_on = on;
}

// Keeps a byte of the client at 1 while a cheat is on, and puts it at 0 once when it goes off.
void KeepByte(uint8_t* base, uint32_t client, uint32_t offset, bool on, bool& was_on) {
  if (!on && !was_on) [[likely]] {
    return;
  }
  if (!client) {
    return;
  }
  if (base[client + offset] != uint8_t(on)) {
    base[client + offset] = uint8_t(on);
  }
  was_on = on;
}

// While the program runs server code between frames (game scripts, and damage, whose kills run the zombie scripts:
// a power-up drop sets client fields), the per-thread client field registry is the server's, as the server sets it
// (see the top of the file); the thread's own comes back after.
class ServerClientFields {
 public:
  ServerClientFields(const PPCContext& ctx, uint8_t* base) : base_(base) {
    const uint32_t block = Load32(base, ctx.r13.u32);
    if (block >= 0x10000) {
      slot_ = block + kTlsClientFields;
      before_ = Load32(base, slot_);
      Store32(base, slot_, kServerClientFields);
    }
  }
  ~ServerClientFields() {
    if (slot_) {
      Store32(base_, slot_, before_);
    }
  }
  ServerClientFields(const ServerClientFields&) = delete;
  ServerClientFields& operator=(const ServerClientFields&) = delete;

 private:
  uint8_t* base_;
  uint32_t slot_ = 0;
  uint32_t before_ = 0;
};

// The game entities G_Spawn can still hand out (see the top of the file): the ones never used below its limit and the
// freed ones.
int FreeEntities(const uint8_t* base) {
  int free = std::max(kSpawnableEntities - int32_t(Load32(base, kLevel + kLevelEntityCount)), 0);
  uint32_t entity = Load32(base, kLevel + kLevelFreeEntity);
  for (uint32_t i = 0; entity >= 0x40000000u && i < kEntityCount; ++i) {
    ++free;
    entity = Load32(base, entity + kEntNextFree);
  }
  return free;
}


// A variant's internal name (empty when the slot holds none).
std::string VariantName(const uint8_t* base, uint32_t index) {
  const uint32_t variant = Load32(base, kWeaponVariants + index * 4);
  const uint32_t name = variant >= 0x40000000u ? Load32(base, variant + kVariantName) : 0;
  if (name < 0x40000000u) {
    return {};
  }
  const char* text = reinterpret_cast<const char*>(base + name);
  return std::string(text, strnlen(text, 64));
}

// The left-hand gun of a dual-wield weapon (see the top of the file), 0 when the weapon has none.
uint32_t LeftHandWeapon(const uint8_t* base, uint32_t index) {
  const std::string name = VariantName(base, index);
  if (name.empty()) {
    return 0;
  }
  std::string without_dw = name;
  if (const size_t dw = without_dw.find("dw"); dw != std::string::npos) {
    without_dw.erase(dw, 2);
  }
  for (uint32_t i = 1; i < 256; ++i) {
    if (i == index) {
      continue;
    }
    std::string other = VariantName(base, i);
    const size_t lh = other.find("lh");
    if (lh == std::string::npos) {
      continue;
    }
    other.erase(lh, 2);
    if (other == name || other == without_dw) {
      return i;
    }
  }
  return 0;
}

// Infinite ammo: the current weapon's clip back to full, and its left-hand gun's (see the top of the file). Main
// thread, from the frame hook; the calls run on a stack area below the hook's own, and ctx is left as it was.
void KeepAmmo(PPCContext& ctx, uint8_t* base, const Actor& actor, PlayerState& state) {
  // The clip size of the last weapon seen, and its left-hand gun (the weapon numbers belong to the match's map).
  auto& sized_weapon = state.sized_weapon;
  auto& size = state.clip_size;
  auto& left_weapon = state.left_weapon;
  auto& left_size = state.left_size;
  if (!state.options.Effective(Option::Ammo)) [[likely]] {
    return;
  }
  const uint32_t client = actor.client;
  if (!client) {
    sized_weapon = 0;
    return;
  }
  if (Load32(base, client + kClientSessionState) != 0) {
    return;
  }
  const uint32_t weapon = Load32(base, client + kClientWeapon);
  if ((weapon & 0xFF) == 0 || (weapon & 0xFF) == 255) {
    return;
  }
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 512) & ~uint64_t(0xF);
  if (weapon != sized_weapon) {
    ctx.r3.u64 = weapon;
    bo2zm::guest::Call<kClipSize>(ctx, base);
    sized_weapon = weapon;
    size = int32_t(ctx.r3.u32);
    left_weapon = 0;
    left_size = 0;
    if (const uint32_t left = LeftHandWeapon(base, weapon & 0xFF)) {
      left_weapon = left;  // as the scripts' setweaponammoclip(name) has it: the index, no attachments
      ctx.r3.u64 = left_weapon;
      bo2zm::guest::Call<kClipSize>(ctx, base);
      left_size = int32_t(ctx.r3.u32);
      REXLOG_INFO("[cheats] infinite ammo: {} has a left-hand gun, {} (clip {})", VariantName(base, weapon & 0xFF),
                  VariantName(base, left), left_size);
    }
  }
  if (size > 0) {
    ctx.r3.u64 = client;
    ctx.r4.u64 = weapon;
    ctx.r5.u64 = uint32_t(size);
    bo2zm::guest::Call<kSetClip>(ctx, base);
  }
  if (left_weapon && left_size > 0) {
    ctx.r3.u64 = client;
    ctx.r4.u64 = left_weapon;
    ctx.r5.u64 = uint32_t(left_size);
    bo2zm::guest::Call<kSetClip>(ctx, base);
  }
  ctx = saved;
}

// Explosive bullets, the client's part (see the top of the file): the frag grenade's weapon (found by name once a
// match), then its explosion effect and sound at each hit. Main thread, from the frame hook.
uint32_t GrenadeDef(const uint8_t* base) {
  static uint32_t cached = 0;
  static bool searched = false;
  static uint64_t epoch = 0;
  if (epoch != g_map_epoch.load(std::memory_order_acquire)) {
    epoch = g_map_epoch.load(std::memory_order_relaxed);
    cached = 0;
    searched = false;
  }
  if (!g_in_match.load(std::memory_order_relaxed)) {
    cached = 0;
    searched = false;
    return 0;
  }
  if (searched) {
    return cached;
  }
  searched = true;
  for (const std::string_view wanted : {"frag_grenade_zm", "sticky_grenade_zm"}) {
    for (uint32_t i = 1; i < 256; ++i) {
      const uint32_t variant = Load32(base, kWeaponVariants + i * 4);
      const uint32_t name = variant >= 0x40000000u ? Load32(base, variant + kVariantName) : 0;
      if (name < 0x40000000u || std::strncmp(reinterpret_cast<const char*>(base + name), wanted.data(),
                                              wanted.size() + 1) != 0) {
        continue;
      }
      cached = Load32(base, variant + kVariantDef);
      REXLOG_INFO("[cheats] explosive bullets: the explosion of {} (weapon {})", wanted, i);
      return cached;
    }
  }
  REXLOG_WARN("[cheats] explosive bullets: no grenade weapon in this match; the hits explode without an effect");
  return 0;
}



void PlayBlasts(PPCContext& ctx, uint8_t* base) {
  if (!g_blasts_waiting.load(std::memory_order_relaxed)) [[likely]] {
    return;
  }
  std::vector<Blast> blasts;
  {
    std::lock_guard lock(g_blasts_mutex);
    g_blasts_waiting.store(false, std::memory_order_relaxed);
    blasts.swap(g_blasts);
  }
  if (blasts.empty()) {
    return;
  }
  const uint32_t def = GrenadeDef(base);
  if (!def) {
    return;
  }
  const uint32_t effect = Load32(base, def + kDefExplosionEffect);
  const uint32_t sound = Load32(base, def + kDefExplosionSound);
  static uint32_t logged_def = 0;
  if (def != logged_def) {
    logged_def = def;
    REXLOG_INFO("[cheats] explosive bullets: effect {:08X}, sound name {:08X} ({})", effect, sound,
                sound >= 0x40000000u ? reinterpret_cast<const char*>(base + sound) : "none");
  }
  const PPCContext saved = ctx;
  const uint32_t sp = (ctx.r1.u32 - 1024) & ~0xFu;
  const uint32_t origin = sp + 256;
  const uint32_t axis = sp + 272;  // forward (up), right, up
  StoreVec(base, axis, {0.0f, 0.0f, 1.0f});
  StoreVec(base, axis + 12, {1.0f, 0.0f, 0.0f});
  StoreVec(base, axis + 24, {0.0f, 1.0f, 0.0f});
  uint32_t alias = 0;
  if (sound >= 0x40000000u && base[sound] != 0) {
    ctx.r1.u64 = sp;
    ctx.r3.u64 = sound;
    bo2zm::guest::Call<kSoundAlias>(ctx, base);
    alias = ctx.r3.u32;
  }
  static int logged_blasts = 0;
  if (logged_blasts < 3) {
    ++logged_blasts;
    REXLOG_INFO("[cheats] explosive bullets: {} explosion(s) played at ({:.0f} {:.0f} {:.0f}), sound alias {:08X}",
                std::min(blasts.size(), kBlastsPerFrame), blasts[0].at.x, blasts[0].at.y, blasts[0].at.z, alias);
  }
  for (size_t i = 0; i < blasts.size() && i < kBlastsPerFrame; ++i) {
    if (!CurrentActor(base, blasts[i].actor)) continue;
    StoreVec(base, origin, blasts[i].at);
    if (effect) {
      ctx.r1.u64 = sp;
      ctx.r3.u64 = effect;
      ctx.r4.u64 = origin;
      ctx.r5.u64 = axis;
      bo2zm::guest::Call<kPlayEffect>(ctx, base);
    }
    if (alias) {
      ctx.r1.u64 = sp;
      ctx.r3.u64 = uint32_t(blasts[i].actor.local);
      ctx.r4.u64 = kEntityWorld;
      ctx.r5.u64 = origin;
      ctx.r6.u64 = 0;
      ctx.r7.u64 = 0;
      ctx.f1.f64 = 1.0;
      ctx.r9.u64 = alias;
      bo2zm::guest::Call<kPlaySound>(ctx, base);
    }
  }
  ctx = saved;
}

// The game's radius damage (see the top of the file) at origin, from attacker, sparing it.
void RadiusDamage(PPCContext& ctx, uint8_t* base, const Vec3& at, uint32_t attacker, float inner, float outer,
                  float radius, uint32_t weapon) {
  const PPCContext saved = ctx;
  const uint32_t sp = (ctx.r1.u32 - 1024) & ~0xFu;
  const uint32_t origin = sp + 256;
  StoreVec(base, origin, at);
  Store32(base, sp + 80, 0);
  Store32(base, sp + 84, attacker);  // spared
  Store32(base, sp + 88, 0);
  Store32(base, sp + 92, kModProjectileSplash);
  Store32(base, sp + 96, 0);
  Store32(base, sp + 100, weapon);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = origin;
  ctx.r4.u64 = attacker;  // inflictor
  ctx.r5.u64 = attacker;
  ctx.r10.u64 = 0;  // no cone
  ctx.f1.f64 = inner;
  ctx.f2.f64 = outer;
  ctx.f3.f64 = radius;
  ctx.f4.f64 = 1.0;
  bo2zm::guest::Call<kRadiusDamage>(ctx, base);
  ctx = saved;
}

// Zombies ignore players: the selected player's ignoreme kept set, and cleared once when the cheat goes off.
void KeepIgnored(uint8_t* base, const Actor& actor, PlayerState& state) {
  bool& was_on = state.ignore_was_on;
  const bool on = state.options.Effective(Option::Ignore);
  if ((!on && !was_on) || !actor.entity) [[likely]] {
    return;
  }
  const uint32_t sentient = Load32(base, actor.entity + kEntSentient);
  if (GuestRange(sentient, kSentientIgnoreMe + 1) && base[sentient + kSentientIgnoreMe] != uint8_t(on)) {
    base[sentient + kSentientIgnoreMe] = uint8_t(on);
  }
  was_on = on;
}

// A live zombie (an actor with health).
bool LiveZombie(const uint8_t* base, uint32_t entity) {
  return Load32(base, entity + kEntActor) != 0 && int32_t(Load32(base, entity + kEntHealth)) > 0;
}

int LiveZombies(const uint8_t* base) {
  int live = 0;
  for (uint32_t i = 1; i < kEntityCount; ++i) {
    live += LiveZombie(base, kGEntities + i * kEntitySize);
  }
  return live;
}

// Every live zombie takes the explosive bullets' radius damage at its origin, from the selected player (see the top of the
// file); the zombies hit.
int KillAllZombies(PPCContext& ctx, uint8_t* base, const Actor& actor) {
  const uint32_t weapon = Load32(base, actor.client + kClientWeapon) & 0xFF;
  const ServerClientFields server(ctx, base);
  int killed = 0;
  for (uint32_t i = 1; i < kEntityCount; ++i) {
    const uint32_t entity = kGEntities + i * kEntitySize;
    if (!LiveZombie(base, entity)) {
      continue;
    }
    const Vec3 origin = LoadVec(base, entity + kEntOrigin);
    RadiusDamage(ctx, base, {origin.x, origin.y, origin.z + 30.0f}, actor.entity, kKillDamage, kKillDamage, 40.0f,
                 weapon == 255 ? 0 : weapon);
    ++killed;
  }
  return killed;
}

// Kill all zombies: once, on request.
void KillZombies(PPCContext& ctx, uint8_t* base) {
  if (!g_kill_zombies.load(std::memory_order_relaxed) || !g_kill_zombies.exchange(false)) [[likely]] {
    return;
  }
  Actor actor;
  { std::lock_guard lock(g_script_mutex); actor = g_kill_target; }
  if (!CurrentActor(base, actor) || !actor.client) {
    REXLOG_INFO("[cheats] kill all zombies: not in a match");
    return;
  }
  REXLOG_INFO("[cheats] kill all zombies: {} hit", KillAllZombies(ctx, base, actor));
}

// A script function argument.
struct ScriptArg {
  enum Kind { kString, kVector, kInt, kFloat } kind;
  std::string text;
  Vec3 vector{};
  int32_t number = 0;
  float real = 0.0f;
};



uint32_t RunObjectThread(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t handle, uint32_t count,
                         uint32_t sp);

// Runs file::function(args...) as a script thread (see the top of the file), on an entity (its gentity) or a script
// object such as a struct (its id) as self, else on the level. The thread's id (1 when the game gives none), or 0 when
// the script or the function is not there.
uint32_t RunScript(PPCContext& ctx, uint8_t* base, std::string_view file, std::string_view function,
                   const std::vector<ScriptArg>& args, uint32_t entity = 0, uint32_t object = 0) {
  const PPCContext saved = ctx;
  const uint32_t area = (ctx.r1.u32 - 4096) & ~0xFu;  // the texts and vectors; the calls' frames go below
  uint32_t cursor = area;
  const auto put_text = [&](std::string_view text) {
    const uint32_t at = cursor;
    std::memcpy(base + at, text.data(), text.size());
    base[at + text.size()] = 0;
    cursor += (uint32_t(text.size()) + 16) & ~15u;
    return at;
  };
  const uint32_t file_at = put_text(file);
  const uint32_t function_at = put_text(function);
  const uint32_t sp = (area - 256) & ~0xFu;
  const ServerClientFields server(saved, base);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = file_at;
  ctx.r5.u64 = function_at;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 1;
  bo2zm::guest::Call<kScriptFunction>(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  if (handle == 0 || handle == Load32(base, kScriptNoFunction)) {
    ctx = saved;
    REXLOG_WARN("[cheats] script {}::{} not found", file, function);
    return 0;
  }
  for (auto it = args.rbegin(); it != args.rend(); ++it) {
    ctx.r1.u64 = sp;
    ctx.r3.u64 = 0;
    switch (it->kind) {
      case ScriptArg::kString:
        ctx.r4.u64 = put_text(it->text);
        bo2zm::guest::Call<kScriptPushString>(ctx, base);
        break;
      case ScriptArg::kVector: {
        const uint32_t at = cursor;
        cursor += 16;
        StoreVec(base, at, it->vector);
        ctx.r4.u64 = at;
        bo2zm::guest::Call<kScriptPushVector>(ctx, base);
        break;
      }
      case ScriptArg::kInt:
        ctx.r4.u64 = uint32_t(it->number);
        bo2zm::guest::Call<kScriptPushInt>(ctx, base);
        break;
      case ScriptArg::kFloat:
        ctx.f1.f64 = it->real;
        bo2zm::guest::Call<kScriptPushFloat>(ctx, base);
        break;
    }
  }
  ctx.r1.u64 = sp;
  ctx.r4.u64 = handle;
  ctx.r5.u64 = args.size();
  if (object) {
    ctx.r3.u64 = RunObjectThread(ctx, base, object, handle, uint32_t(args.size()), sp);
  } else if (entity) {
    ctx.r3.u64 = entity;
    bo2zm::guest::Call<kScriptRunEntityThread>(ctx, base);
  } else {
    ctx.r3.u64 = 0;
    bo2zm::guest::Call<kScriptRunThread>(ctx, base);
  }
  const uint32_t thread = ctx.r3.u32;
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = thread;
  bo2zm::guest::Call<kScriptFreeThread>(ctx, base);
  ctx = saved;
  return thread ? thread : 1;
}

// A perk's index (sub_82690E50), or -1. Looked up once per name.
int PerkIndex(PPCContext& ctx, uint8_t* base, std::string_view perk) {
  static std::vector<std::pair<std::string, int>> known;
  for (const auto& [name, index] : known) {
    if (name == perk) {
      return index;
    }
  }
  const PPCContext saved = ctx;
  const uint32_t text = (ctx.r1.u32 - 1024) & ~0xFu;
  std::memcpy(base + text, perk.data(), perk.size());
  base[text + perk.size()] = 0;
  ctx.r1.u64 = (text - 256) & ~0xFu;
  ctx.r3.u64 = text;
  bo2zm::guest::Call<kPerkIndex>(ctx, base);
  const int index = int(ctx.r3.u32) < 63 ? int(ctx.r3.u32) : -1;
  ctx = saved;
  known.emplace_back(perk, index);
  return index;
}

bool HasPerk(const uint8_t* base, uint32_t client, int index) {
  return index >= 0 && (Load32(base, client + kPsPerks + uint32_t(index >> 5) * 4) & (1u << (index & 31))) != 0;
}

// Gives the selected player a perk the game's way, unless they have it.
void GivePerk(PPCContext& ctx, uint8_t* base, const Actor& actor, std::string_view perk) {
  const uint32_t client = actor.client;
  const int index = PerkIndex(ctx, base, perk);
  if (index < 0) {
    REXLOG_WARN("[cheats] perk {}: not a perk", perk);
    return;
  }
  if (HasPerk(base, client, index)) {
    return;
  }
  if (RunScript(ctx, base, kPerksScript, "give_perk", {{ScriptArg::kString, std::string(perk)},
                                                        {ScriptArg::kInt, {}, {}, 0}}, actor.entity)) {
    REXLOG_INFO("[cheats] perk {}", perk);
  }
}

// Player 0 is up: normal movement (not down, being revived, dead or spectating).
bool IsUp(const uint8_t* base, uint32_t client) {
  const uint32_t type = Load32(base, client + kPsPmType);
  return type == kPmNormal || type == kPmNormalLinked;
}

// Takes every zombies perk the selected player has the game's way: perk_think (maps/mp/zombies/_zm_perks) ends a perk on the
// player's notify "<perk>_stop" (as lose_random_perk does) and undoes it (the bit, HUD icon, health); the notify comes
// from common_scripts/utility::delay_notify(notify, delay) on the player, a server frame later.
void RemovePerks(PPCContext& ctx, uint8_t* base, const Actor& actor) {
  const uint32_t client = actor.client;
  auto& state = StateForActor(actor);
  int removed = 0;
  for (const char* perk : kZombiePerks) {
    if (!HasPerk(base, client, PerkIndex(ctx, base, perk))) {
      continue;
    }
    if (RunScript(ctx, base, kUtilityScript, "delay_notify",
                  {{ScriptArg::kString, std::string(perk) + "_stop"}, {ScriptArg::kFloat, {}, {}, 0, 0.05f}},
                  actor.entity)) {
      ++removed;
    }
  }
  // Keep perks forgets them (it must not give them back after the next time down).
  state.noted_perks[0] = state.noted_perks[1] = 0;
  state.perks_gone_since = {};
  state.perks_quiet_until = std::chrono::steady_clock::now() + kForgetPerkTime;
  REXLOG_INFO("[cheats] remove perks: {} taken", removed);
}

// Keep perks (see the top of the file): Server thread, every server frame while it is on.
void KeepPerks(PPCContext& ctx, uint8_t* base, const Actor& actor, PlayerState& state) {
  auto& was_down = state.perks_was_down;
  auto& active = state.perks_active;
  const uint32_t client = state.options.Effective(Option::KeepPerks) ? actor.client : 0;
  if (!client) [[likely]] {
    if (active) {
      active = was_down = false;
      state.noted_perks[0] = state.noted_perks[1] = 0;
      state.perks_gone_since = state.perks_quiet_until = {};
    }
    return;
  }
  active = true;
  if (!IsUp(base, client)) {
    was_down = true;  // down, being revived or dead: what they had stays noted
    return;
  }
  if (was_down) {
    was_down = false;
    for (const char* perk : kZombiePerks) {
      const int index = PerkIndex(ctx, base, perk);
      if (index >= 0 && (state.noted_perks[index >> 5] & (1u << (index & 31))) != 0) {
        GivePerk(ctx, base, actor, perk);
      }
    }
  }
  const uint32_t now[2] = {Load32(base, client + kPsPerks), Load32(base, client + kPsPerks + 4)};
  const auto clock_now = std::chrono::steady_clock::now();
  if (clock_now < state.perks_quiet_until) {  // REMOVE PERKS: the perks are on their way out
    return;
  }
  // Every perk seen while up stays noted (the game takes them in the same frame it puts the player down, sometimes
  // before the player state shows it); one gone for kForgetPerkTime while the player stays up was taken some other
  // way, and is forgotten.
  if (((state.noted_perks[0] & ~now[0]) | (state.noted_perks[1] & ~now[1])) != 0) {
    if (state.perks_gone_since == std::chrono::steady_clock::time_point{}) {
      state.perks_gone_since = clock_now;
    } else if (clock_now - state.perks_gone_since >= kForgetPerkTime) {
      state.noted_perks[0] = state.noted_perks[1] = 0;
      state.perks_gone_since = {};
    }
  } else {
    state.perks_gone_since = {};
  }
  state.noted_perks[0] |= now[0];
  state.noted_perks[1] |= now[1];
}

// Outside a match no server frame runs to take the menu's script requests (ServerFrame): main thread, they are
// dropped as the server frame would drop them, and the round shown is 0.
void DropScriptRequests() {
  g_round_shown.store(0, std::memory_order_relaxed);
  if (g_kill_zombies.load(std::memory_order_relaxed) && g_kill_zombies.exchange(false)) {
    REXLOG_INFO("[cheats] kill all zombies: not in a match");
  }
  if (!g_script_waiting.load(std::memory_order_relaxed) && !g_world_waiting.load(std::memory_order_relaxed) &&
      !g_player_waiting.load(std::memory_order_relaxed))
      [[likely]] {
    return;
  }
  {
    std::lock_guard lock(g_script_mutex);
    g_script_waiting.store(false, std::memory_order_relaxed);
    g_world_waiting.store(false, std::memory_order_relaxed);
    g_player_waiting.store(false, std::memory_order_relaxed);
    g_player_requests.clear();
    g_round_request = 0;
    g_doors_request = g_power_request = g_debris_request = 0;
  }
  REXLOG_INFO("[cheats] power-ups, perks, rounds, weapons, doors and power: not in a match");
}

// Power-up drops, perks and round changes asked for from the menu, run between server frames.
void RunScriptRequests(PPCContext&, uint8_t* base) {
  if (!g_script_waiting.load(std::memory_order_relaxed)) return;
  int round = 0;
  Actor target;
  {
    std::lock_guard lock(g_script_mutex);
    g_script_waiting.store(false, std::memory_order_relaxed);
    std::swap(round, g_round_request);
    target = g_round_target;
  }
  if (round > 0 && target.client && CurrentActor(base, target)) {
    g_pending_round = round;
    g_pending_round_target = target;
  }
}

// The id the game linked for a field name in a loaded script (see the top of the file), or 0.
uint16_t LinkedFieldId(PPCContext& ctx, uint8_t* base, std::string_view file, std::string_view field) {
  const PPCContext saved = ctx;
  const uint32_t text = (ctx.r1.u32 - 1024) & ~0xFu;
  std::memcpy(base + text, file.data(), file.size());
  base[text + file.size()] = 0;
  ctx.r1.u64 = (text - 256) & ~0xFu;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = text;
  ctx.r5.u64 = 1;
  ctx.r6.u64 = 1;
  bo2zm::guest::Call<kScriptFile>(ctx, base);
  const uint32_t header = ctx.r3.u32;
  ctx = saved;
  if (header < 0x40000000u) {
    return 0;
  }
  const uint32_t count = Load16(base, header + 0x32);
  uint32_t entry = header + Load32(base, header + 0x18);
  for (uint32_t i = 0; i < count && i < 0x4000; ++i) {
    const uint16_t string = Load16(base, entry);
    const uint8_t uses = base[entry + 2];
    const uint32_t name = header + string;
    if (base[entry + 3] == kFixupFieldName && uses > 0 &&
        std::memcmp(base + name, field.data(), field.size()) == 0 && base[name + field.size()] == 0) {
      const uint16_t id = Load16(base, header + Load32(base, entry + 4));
      return id != string ? id : 0;  // still the string's offset: not linked
    }
    entry += 4 + 4 * uint32_t(uses);
  }
  return 0;
}

// The value slot of level.<field> when it holds an int (see the top of the file), or 0.
uint32_t LevelIntSlot(PPCContext& ctx, uint8_t* base, uint16_t field) {
  const uint32_t level = Load32(base, kScrVarPub + kScrLevelObject);
  const uint32_t objects = Load32(base, kScrVarGlob + kScrObjects);
  const uint32_t values = Load32(base, kScrVarGlob + kScrValues);
  if (field == 0 || level == 0 || objects < 0x40000000u || values < 0x40000000u ||
      (Load32(base, objects + level * 8 + 4) & 0x7F) > kScrFieldObjectTypes) {
    return 0;
  }
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 256) & ~0xFu;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = level;
  ctx.r5.u64 = field;
  bo2zm::guest::Call<kScriptGetVariable>(ctx, base);
  const uint32_t variable = ctx.r3.u32 & 0xFFFF;
  ctx = saved;
  const uint32_t slot = values + variable * 16;
  return variable != 0 && base[slot + 6] == kScrInt ? slot : 0;
}

// The server's parent script variables in use (see kServerParentVariables): the count less the free list's length.
// Server thread (the list changes under the scripts); -1 before the variables exist.
int ParentsInUse(const uint8_t* base) {
  const uint32_t table = Load32(base, kScrVarGlob + kScrObjects);
  if (table < 0x40000000u) {
    return -1;
  }
  uint32_t free = 0;
  for (uint32_t i = Load16(base, table + 2); i != 0 && free < kServerParentVariables;
       i = Load16(base, table + i * 8 + 2)) {
    ++free;
  }
  return int(kServerParentVariables - free);
}

// The parent variables in use, about once a second for ScriptParentsInUse, and the highest of each minute in the log.
// Server thread.
void ReportParents(const uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point counted{};
  static Clock::time_point reported = Clock::now();
  static int peak = -1;
  const auto now = Clock::now();
  if (now - counted < std::chrono::seconds(1)) [[likely]] {
    return;
  }
  counted = now;
  const int in_use = ParentsInUse(base);
  g_parents_in_use.store(in_use, std::memory_order_relaxed);
  peak = std::max(peak, in_use);
  if (now - reported >= kParentsReportEvery) {
    reported = now;
    REXLOG_INFO("[cheats] script variables: {} of the server's {} parent variables in use, at most {} in the last "
                "minute", in_use, kServerParentVariables, peak);
    peak = -1;
  }
}

// The round picker (see the top of the file): Server thread, every server frame. Also logs the game's round counter
// when it changes, so a test log shows the rounds.
void KeepRoundRequest(PPCContext& ctx, uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  static uint32_t shown_round = 0;
  static Clock::time_point checked_at{};
  static int waiting_for = 0;  // the pick waiting (logged once)
  static int changing_to = 0;  // the change made, until the game's round counter shows it
  static Clock::time_point changed_at{};
  static uint64_t epoch = 0;
  if (epoch != g_map_epoch.load(std::memory_order_acquire)) {
    epoch = g_map_epoch.load(std::memory_order_relaxed);
    shown_round = 0;
    checked_at = changed_at = {};
    waiting_for = changing_to = 0;
  }
  const uint32_t client = Client(base);
  const uint32_t counter = client ? Load32(base, kLevel + kLevelRoundsPlayed) : 0;
  g_round_shown.store(int(counter), std::memory_order_relaxed);
  if (counter != shown_round) {
    shown_round = counter;
    if (counter != 0) {
      REXLOG_INFO("[cheats] the game is on round {}", counter);
    }
  }
  if (changing_to != 0 &&
      (!client || int(counter) == changing_to || Clock::now() - changed_at > kRoundChangeTimeout)) {
    changing_to = 0;
  }
  if (g_pending_round != 0 && !CurrentActor(base, g_pending_round_target)) g_pending_round = 0;
  if (g_pending_round == 0) [[likely]] {
    return;
  }
  if (!client) {
    g_pending_round = waiting_for = 0;
    return;
  }
  const int round = g_pending_round;
  if (round == waiting_for && Clock::now() - checked_at < kRoundCheckEvery) {
    return;  // a pick that is waiting
  }
  checked_at = Clock::now();
  const auto wait = [&](const char* why) {
    if (waiting_for != round) {
      waiting_for = round;
      REXLOG_INFO("[cheats] round {}: {}", round, why);
    }
  };
  const auto drop = [&](const char* why) {
    REXLOG_INFO("[cheats] round {}: {}", round, why);
    g_pending_round = waiting_for = 0;
  };
  // Picks made while a change is under way (A pressed again and again) wait for it; the same round again is done.
  if (changing_to != 0) {
    if (round == changing_to) {
      drop("already on its way");
    } else {
      wait("waiting for the round change under way");
    }
    return;
  }
  const uint16_t round_field = LinkedFieldId(ctx, base, kZmScript, "round_number");
  const uint16_t total_field = LinkedFieldId(ctx, base, kZmScript, "zombie_total");
  const uint32_t round_now = LevelIntSlot(ctx, base, round_field);
  const uint32_t total_now = LevelIntSlot(ctx, base, total_field);
  if (!round_now || !total_now) {
    REXLOG_WARN("[cheats] round {}: level.round_number or level.zombie_total not found (field ids {:04X} {:04X})",
                round, round_field, total_field);
    g_pending_round = waiting_for = 0;
    return;
  }
  // The round on now (between rounds: the next one, already counted): nothing to do.
  if (int32_t(Load32(base, round_now)) == round) {
    drop("already this round");
    return;
  }
  // Between rounds (or before the first) nothing is left to end: wait for the next round to start.
  if (int32_t(Load32(base, total_now)) <= 0 && LiveZombies(base) == 0) {
    wait("between rounds; waiting for the next round to start");
    return;
  }
  g_pending_round = waiting_for = 0;
  const int parents_before = ParentsInUse(base);
  const int killed = KillAllZombies(ctx, base, g_pending_round_target);
  // The kills ran the zombie scripts: the slots are looked up again for the writes.
  const uint32_t total_slot = LevelIntSlot(ctx, base, total_field);
  const uint32_t round_slot = LevelIntSlot(ctx, base, round_field);
  if (!total_slot || !round_slot) {
    REXLOG_WARN("[cheats] round {}: the level fields went away", round);
    return;
  }
  Store32(base, total_slot, 0);
  Store32(base, round_slot, uint32_t(round - 1));
  changing_to = round;
  changed_at = Clock::now();
  REXLOG_INFO("[cheats] round {}: {} zombies killed, zombie_total 0, round_number {}: this round ends and the next "
              "is round {} (parent script variables in use {} before the kills, {} after)", round, killed, round - 1,
              round, parents_before, ParentsInUse(base));
}

// Restores the stock jump constant; personal heights never write shared game data.
void RestoreStockJump(uint8_t* base) {
  static std::mutex write_mutex;
  std::lock_guard lock(write_mutex);
  if (LoadFloat(base, kJumpHeight) == kJumpLevels[0]) return;
  auto* runtime = rex::Runtime::instance();
  auto* heap = runtime && runtime->memory() ? runtime->memory()->LookupHeap(kJumpHeight) : nullptr;
  uint32_t old = 0;
  if (!heap || !heap->Protect(kJumpHeight, 4, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old)) return;
  StoreFloat(base, kJumpHeight, kJumpLevels[0]);
  heap->Protect(kJumpHeight, 4, old);
}

// The combo's presses, counted on the polling thread: each one turns the chosen fly mode on or off.
void TakeComboPresses(PlayerState& state, uint64_t revision) {
  if (const int mode = state.options.TakeCombo(revision); mode >= 0) {
    REXLOG_INFO("[cheats] combo: fly mode {}", FlyName(mode));
  }
}

// Brings the held dvars to their values (and released ones back to the game's), and shows the menu's picks, in one
// console run.
void KeepHeldDvars(PPCContext& ctx, uint8_t* base) {
  static int frames = 0;
  if (!g_held_dirty.load(std::memory_order_relaxed) &&
      (!g_held_any.load(std::memory_order_relaxed) || ++frames < kCheckEveryFrames)) [[likely]] {
    return;
  }
  std::string commands;
  std::vector<uint32_t> pointers;
  {
    std::lock_guard lock(g_held_mutex);
    frames = 0;
    g_held_dirty.store(false, std::memory_order_relaxed);
    for (const auto& [name, shown] : g_shown) {
      commands += "set bo2zm_hold_" + name + " " + shown + "; ";
    }
    g_shown.clear();
    for (auto it = g_held.begin(); it != g_held.end();) {
      if (!it->dvar) {
        it->dvar = bo2zm::dvars::Find(it->name);
      }
      const auto& dvar = it->dvar;
      if (!dvar) {  // not registered yet (the game registers some only with a map): try again later
        it = it->release ? g_held.erase(it) : it + 1;
        continue;
      }
      if (it->original.empty()) {
        it->original = bo2zm::dvars::Text(base, *dvar);
      }
      const std::string& goal = it->release ? it->original : it->value;
      if (!bo2zm::dvars::Matches(base, *dvar, goal)) {
        if (it->release) {
          REXLOG_INFO("[cheats] {} back to the game's {}", it->name, goal);
        } else if (it->fixes++ == 0) {
          REXLOG_INFO("[cheats] {} = {} (the game's: {})", it->name, goal, bo2zm::dvars::Text(base, *dvar));
        } else if (it->fixes <= 4) {
          REXLOG_INFO("[cheats] {} was changed to {} (map load or script): {} again", it->name,
                      bo2zm::dvars::Text(base, *dvar), goal);
        }
        commands += "set " + it->name + " " + goal + "; ";
        pointers.push_back(dvar->pointer);
      }
      it = it->release ? g_held.erase(it) : it + 1;
    }
    g_held_any.store(!g_held.empty(), std::memory_order_relaxed);
  }
  if (!commands.empty()) {
    bo2zm::dvars::ExecuteIgnoringCheats(ctx, base, commands, pointers);
  }
}

// A map's first visit compiles its pipelines for a minute or more on a Switch, and their draws are skipped until each
// is ready (bo2zm::preset::FirstVisitBuilding): meanwhile, in a game this console runs for its own players only, the
// game's clock is held almost still (timescale), so nothing happens while the screen is dark. Not with other consoles
// in the game (their players would wait as well), and not while the menu holds timescale itself.
constexpr uint16_t kGamePortForPeers = 1000;  // BO2's game port: another console in the game talks to it

void SlowWhileBuilding(const uint8_t* base) {
  static bool slowed = false;
  bool slow = REXCVAR_GET(bo2zm_first_visit_wait) && bo2zm::preset::FirstVisitBuilding() &&
              g_in_match.load(std::memory_order_relaxed) && LocalServerRunning(base);
  if (slow) {
    uint32_t peer = 0;
    int64_t age = 0;
    if (rex::system::system_link::LastPeer(kGamePortForPeers, &peer, &age) && age < 5000) slow = false;
  }
  if (slow == slowed) return;
  std::lock_guard lock(g_held_mutex);
  for (const Held& h : g_held) {
    if (h.name == "timescale" && h.menu && !h.release) {
      slowed = false;  // the player's pick wins, and stays
      return;
    }
  }
  slowed = slow;
  HoldLocked("timescale", "0.02", slow);
  REXLOG_INFO("[cheats] first visit to the map: the game's clock {}", slow ? "held while its graphics are prepared"
                                                                         : "runs again");
}

// Logs sv_cheats when it changes, so a test log shows whether the game ever turns it off. Once a second.
void WatchSvCheats(const uint8_t* base) {
  static int frames = 0;
  static std::optional<bo2zm::dvars::Dvar> dvar;
  static uint32_t last = ~0u;
  if (++frames < 60) [[likely]] {
    return;
  }
  frames = 0;
  if (!dvar && !(dvar = bo2zm::dvars::Find("sv_cheats"))) {
    return;
  }
  const uint32_t now = Load32(base, dvar->pointer + kDvarCurrent);
  if (now != last) {
    last = now;
    REXLOG_INFO("[cheats] sv_cheats is {}", bo2zm::dvars::Text(base, *dvar));
  }
}

// UFO: the velocity for this step, from the stick and the camera (see the top of the file).
Vec3 UfoVelocity(const uint8_t* base, uint32_t pm, uint32_t ps, float dt) {
  constexpr float kRadians = 3.14159265f / 180.0f;
  const float pitch = LoadFloat(base, ps + kPsViewAngles) * kRadians;
  const float yaw = LoadFloat(base, ps + kPsViewAngles + 4) * kRadians;
  const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
  const Vec3 forward = {cp * cy, cp * sy, -sp};  // pitch is positive looking down
  const Vec3 right = {sy, -cy, 0.0f};
  const float stick_forward = float(int8_t(base[pm + kPmForward])) / 127.0f;
  const float stick_right = float(int8_t(base[pm + kPmRight])) / 127.0f;
  Vec3 wish = {forward.x * stick_forward + right.x * stick_right, forward.y * stick_forward + right.y * stick_right,
               forward.z * stick_forward};
  const float length = std::sqrt(Dot(wish, wish));
  if (length > 1.0f) {
    wish = {wish.x / length, wish.y / length, wish.z / length};
  }
  const Vec3 old = LoadVec(base, ps + kPsVelocity);
  const float keep = std::exp(-kUfoDrag * dt);
  return {(old.x + wish.x * kUfoAccel * dt) * keep, (old.y + wish.y * kUfoAccel * dt) * keep,
          (old.z + wish.z * kUfoAccel * dt) * keep};
}

// What god mode did, for test runs where the player still went down: its state when it changes, and every 30 s in a
// match the hits the damage hook dropped and the selected player's health; a fall in health while it is on (a hit that came
// another way) is logged at once, the first 20 times.
void ReportGod(uint8_t* base, const Actor& actor, PlayerState& state) {
  using Clock = std::chrono::steady_clock;
  auto& was_on = state.reported_god;
  auto& last_health = state.last_health;
  auto& falls_logged = state.health_falls_logged;
  auto& next = state.next_god_report;
  const bool on = state.options.Effective(Option::God);
  if (on != was_on) {
    was_on = on;
    REXLOG_INFO("[cheats] {} god mode {}", ActorName(actor), on ? "on" : "off");
  }
  if (!actor.client) {
    last_health = 0;
    return;
  }
  const int32_t health = int32_t(Load32(base, actor.entity + kEntHealth));
  if (on && health < last_health && falls_logged < 20) {
    ++falls_logged;
    REXLOG_WARN("[cheats] {} god mode on, health fell from {} to {}", ActorName(actor), last_health, health);
  }
  last_health = health;
  const Clock::time_point now = Clock::now();
  if (now >= next) {
    if (next != Clock::time_point{}) {
      REXLOG_INFO("[cheats] {} god mode {}: {} hits dropped in 30 s; health {}", ActorName(actor), on ? "on" : "off",
                  state.god_blocked.exchange(0, std::memory_order_relaxed), health);
    }
    next = now + std::chrono::seconds(30);
  }
}

// ---- The map's catalog, the gun giver, doors and power (see the top of the file) ----

constexpr uint32_t kVariantDisplayName = 0xC;  // a localization key (the weapon field table: displayName at 0xC)
constexpr uint32_t kDefWeaponClass = 0x20;     // weaponClass: 0x2EC in the field table, which counts from 0x2CC
constexpr uint32_t kDefInventoryType = 0x2C;   // inventoryType: 0x2F8
constexpr uint32_t kInventoryPrimary = 0;
constexpr uint32_t kEntInUse = 251;            // a byte: getentarray (sub_8237CCB0) skips entities without it
constexpr uint32_t kEntTarget = 0x178;         // string ids (the entity field table at 0x82036AC0)
constexpr uint32_t kEntTargetName = 0x17A;
constexpr uint32_t kEntNoteworthy = 0x17C;
constexpr uint32_t kEntAngles = 0x140;
constexpr uint32_t kStringTable = 0x83EEF324;  // SL_ConvertToString (sub_82532A18): the text of id n at *this + n * 24 + 4
constexpr uint32_t kHasWeapon = 0x821A8C38;    // call only: does a player state have a weapon
constexpr uint32_t kTakeWeapon = 0x826B42B8;   // call only: takes a weapon from a player state
constexpr uint32_t kClientLastWeapon = 21572;  // the takeweapon method clears it with the weapon in hand
constexpr uint32_t kNotifyEntity = 0x8253CF78;  // call only: Scr_NotifyNum(entity number, class 0, string id, count)
constexpr uint32_t kScriptPushEntity = 0x8253D940;  // call only: pushes an entity (instance, number, class, r6)
constexpr uint32_t kScriptEntityContext = 0x82976AC8;  // what the game's entity pushes pass in r6 (sub_82403C88)
constexpr uint32_t kScriptEntityObject = 0x82536580;   // call only: an entity's script object (Scr_GetEntityId)
constexpr const char* kWeaponsScript = "maps/mp/zombies/_zm_weapons";
constexpr const char* kBlockersScript = "maps/mp/zombies/_zm_blockers";
constexpr const char* kGameModuleScript = "maps/mp/zombies/_zm_game_module";
constexpr uint8_t kFixupString = 0;
constexpr uint32_t kEntModel = 0x16C;  // a model index
constexpr size_t kDvarText = 240;       // the longest catalog part in one game dvar (the menu reads 255 at most)

// A string id's text (empty for none).
std::string_view IdText(const uint8_t* base, uint16_t id) {
  const uint32_t table = Load32(base, kStringTable);
  if (id == 0 || table < 0x40000000u) {
    return {};
  }
  const char* text = reinterpret_cast<const char*>(base + table + uint32_t(id) * 24 + 4);
  return std::string_view(text, strnlen(text, 96));
}

// The variant index of a weapon by its internal name, 0 when the match has none (never registered).
uint32_t VariantIndex(const uint8_t* base, std::string_view name) {
  for (uint32_t i = 1; i < 256; ++i) {
    const uint32_t variant = Load32(base, kWeaponVariants + i * 4);
    const uint32_t text = variant >= 0x40000000u ? Load32(base, variant + kVariantName) : 0;
    if (text >= 0x40000000u && std::strncmp(reinterpret_cast<const char*>(base + text), name.data(), name.size()) == 0 &&
        base[text + name.size()] == 0) {
      return i;
    }
  }
  return 0;
}

uint32_t VariantDef(const uint8_t* base, uint32_t index) {
  const uint32_t variant = Load32(base, kWeaponVariants + (index & 0xFF) * 4);
  return variant >= 0x40000000u ? Load32(base, variant + kVariantDef) : 0;
}

// A primary weapon (inventoryType 0; the logged catalog has 2 for the Jet Gun, an item, 3 for alternate modes such as
// the grenade launchers, 5 for dual-wield left hands).
bool IsPrimary(const uint8_t* base, uint32_t index) {
  const uint32_t def = VariantDef(base, index);
  return def >= 0x40000000u && Load32(base, def + kDefInventoryType) == kInventoryPrimary;
}

// The Pack-a-Punch version of a gun among the registered weapons (names by variant index), or empty: "<gun>_zm" ->
// "<gun>_upgraded_zm", the scripts' usual naming, else a registered primary "<gun>_..._upgraded_zm" (the M16's is
// m16_gl_upgraded_zm, for example).
std::string UpgradedName(const uint8_t* base, const std::vector<std::string>& names, std::string_view name) {
  constexpr std::string_view kSuffix = "_zm";
  constexpr std::string_view kUpgraded = "_upgraded_zm";
  if (name.size() <= kSuffix.size() || name.substr(name.size() - kSuffix.size()) != kSuffix) {
    return {};
  }
  const std::string gun(name.substr(0, name.size() - kSuffix.size()));
  if (std::find(names.begin(), names.end(), gun + std::string(kUpgraded)) != names.end()) {
    return gun + std::string(kUpgraded);
  }
  for (uint32_t i = 1; i < names.size(); ++i) {
    const std::string_view other = names[i];
    if (other.size() > gun.size() + kUpgraded.size() && other.substr(0, gun.size() + 1) == gun + "_" &&
        other.substr(other.size() - kUpgraded.size()) == kUpgraded && IsPrimary(base, i)) {
      return std::string(other);
    }
  }
  return {};
}

// The menu's gun categories, in the order the menu shows them.
enum GunCategory { kPistol, kSmg, kRifle, kShotgun, kLmg, kSniper, kLauncher, kSpecial, kNotAGun };
constexpr const char* kCategoryNames[] = {"pistol", "smg", "rifle", "shotgun", "lmg", "sniper", "launcher", "special"};

// A primary weapon's category for the menu: by name for the wonder weapons, the snipers and what is not a gun to give
// (power-up and quest weapons, hands, tools), else by its weaponClass. We believe the classes follow the T6 enum
// order (rifle 0, mg 1, smg 2, spread 3, pistol 4, grenade 5, rocket launcher 6, ..., pistol spread 13); the catalog
// agrees: the M14 is 0, the RPD 1, the MP5 2, the Olympia 3, the M1911 4, the War Machine (a grenade launcher) 5, the
// RPG 6 and the Executioner 13.
GunCategory Category(std::string_view name, uint32_t weapon_class) {
  const auto has = [&](std::string_view part) { return name.find(part) != std::string_view::npos; };
  for (const std::string_view skip :
       {"upgraded", "defaultweapon", "zombie_", "minigun", "syrette", "bottle", "knuckle", "fists", "tazer", "bowie_knife", "knife_zm",
        "sickle", "riotshield", "equip_", "hands", "chalk", "screecher", "death_throe", "no_melee", "staff_revive",
        "time_bomb", "beacon", "tomahawk", "spoon", "spork", "one_inch", "falling", "flourish", "builder", "_lh"}) {
    if (has(skip)) {
      return kNotAGun;
    }
  }
  for (const std::string_view special : {"ray_gun", "raygun_mark2", "jetgun", "slipgun", "slowgun", "blundergat",
                                          "blundersplat", "staff_", "knife_ballistic", "thundergun"}) {
    if (has(special)) {
      return kSpecial;
    }
  }
  for (const std::string_view sniper : {"dsr50", "barretm82", "svu", "ballista"}) {
    if (has(sniper)) {
      return kSniper;
    }
  }
  switch (weapon_class) {
    case 0: return kRifle;
    case 1: return kLmg;
    case 2: return kSmg;
    case 3: return kShotgun;
    case 4: case 13: return kPistol;
    case 5: case 6: return kLauncher;
    default: return kNotAGun;
  }
}

struct Gun {
  std::string name;
  std::string display;  // the localization key
  GunCategory category;
  bool upgradable;
};

// The id the game linked for a string or field name in a loaded script (see the top of the file), or 0.
uint16_t LinkedId(PPCContext& ctx, uint8_t* base, std::string_view file, std::string_view text, uint8_t kind) {
  const PPCContext saved = ctx;
  const uint32_t at = (ctx.r1.u32 - 1024) & ~0xFu;
  std::memcpy(base + at, file.data(), file.size());
  base[at + file.size()] = 0;
  ctx.r1.u64 = (at - 256) & ~0xFu;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = at;
  ctx.r5.u64 = 1;
  ctx.r6.u64 = 1;
  bo2zm::guest::Call<kScriptFile>(ctx, base);
  const uint32_t header = ctx.r3.u32;
  ctx = saved;
  if (header < 0x40000000u) {
    return 0;
  }
  const uint32_t count = Load16(base, header + 0x32);
  uint32_t entry = header + Load32(base, header + 0x18);
  for (uint32_t i = 0; i < count && i < 0x4000; ++i) {
    const uint16_t string = Load16(base, entry);
    const uint8_t uses = base[entry + 2];
    const uint32_t name = header + string;
    if (base[entry + 3] == kind && uses > 0 && std::memcmp(base + name, text.data(), text.size()) == 0 &&
        base[name + text.size()] == 0) {
      const uint16_t id = Load16(base, header + Load32(base, entry + 4));
      return id != string ? id : 0;  // still the string's offset: not linked
    }
    entry += 4 + 4 * uint32_t(uses);
  }
  return 0;
}

// The power-ups each map's include_powerups() (maps/mp/<map>.gsc, in the map's patch zone) includes, as read from the
// zones. The menu goes by this table rather than by level.zombie_include_powerups.
constexpr std::pair<std::string_view, std::string_view> kMapPowerups[] = {
    {"zm_transit", "nuke insta_kill double_points full_ammo carpenter minigun"},
    {"zm_nuked", "nuke insta_kill double_points full_ammo fire_sale"},
    {"zm_highrise", "nuke insta_kill double_points full_ammo carpenter free_perk"},
    {"zm_prison", "nuke insta_kill double_points full_ammo fire_sale"},
    {"zm_buried", "nuke insta_kill double_points full_ammo carpenter fire_sale free_perk random_weapon"},
    {"zm_tomb", "nuke insta_kill double_points full_ammo fire_sale free_perk zombie_blood bonus_points_player"},
};

// The zombies perks' specialty names with the map's machines for them (script_noteworthy of the "zombie_vending"
// triggers, the entities standard_powered_items in _zm_power.gsc powers).
constexpr const char* kVendingName = "zombie_vending";

std::mutex g_catalog_mutex;
std::vector<std::string> g_map_weapons;  // the registered weapons' names by variant index (Server thread reads them)
std::vector<std::string> g_map_guns;     // the menu's guns: the only ones bo2zm_give_weapon gives
std::string g_flag_perks;                // the perks whose level.zombiemode_using_*_perk is set (ReadPerkFlags)
// The menu's catalog as this console's match has it, for the consoles that join it (CatalogText): guns, perks and
// power-ups, with a revision that changes with them. Under g_catalog_mutex.
std::string g_catalog_text;
uint32_t g_catalog_revision = 0;

// Logs the guest memory left, once at the menus and once per match: each guest heap's size and the pages nobody has
// reserved (the physical views at 0xA0000000, 0xC0000000 and 0xE0000000 share the console's physical memory), and the
// word sub_8249BC78 returns (0x83D96030), the value the zone loader's "Could not allocate %.2f MB of type '%s' for
// zone '%s' needed an additional %.2f MB" (DB_AllocXZoneMemory, sub_822C54D0) prints last. That word looks like an
// address rather than a byte count.
void LogZoneMemory(const uint8_t* base, std::string_view where) {
  constexpr uint32_t kZoneMemoryWord = 0x83D96030;
  std::string heaps;
  auto* memory = rex::Runtime::instance()->memory();
  for (const uint32_t address : {0x00000000u, 0x40000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    if (auto* heap = memory->LookupHeap(address)) {
      const double page_mb = heap->page_size() / 1048576.0;
      heaps += fmt::format(" {:08X}: {:.1f} of {:.1f} MB free;", address, heap->GetUnreservedPageCount() * page_mb,
                           heap->GetTotalPageCount() * page_mb);
    }
  }
  REXLOG_INFO("[cheats] guest memory {}:{} the zone loader's word at 0x83D96030 is {:08X}", where, heaps,
              Load32(base, kZoneMemoryWord));
}

// Builds the menu's catalog of this match (main thread, every 2 s in a match) and shows what changed in game dvars:
// bo2zm_guns_ui_count and bo2zm_guns_ui_<n> ("name,key,category,upgradable" with spaces between, at most
// kDvarText characters each, since the menu reads at most 255 and cuts a longer value),
// bo2zm_map_perks_ui and bo2zm_map_powerups_ui (names with spaces between).
void UpdateCatalog(PPCContext& ctx, uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  static std::string shown_guns, shown_perks, shown_powerups;
  static bool logged_weapons = false;
  const uint32_t client = Client(base);
  static bool logged_menus = false;
  if (!client && !logged_menus && Load32(base, 0x83D96030) != 0) {
    logged_menus = true;
    LogZoneMemory(base, "at the menus");
  }
  if (!client) {
    if (!shown_guns.empty() || logged_weapons) {
      shown_guns.clear();
      shown_perks.clear();
      shown_powerups.clear();
      logged_weapons = false;
      std::lock_guard lock(g_catalog_mutex);
      g_map_weapons.clear();
      g_map_guns.clear();
      g_flag_perks.clear();
      g_catalog_text.clear();
    }
    return;
  }
  const auto now = Clock::now();
  if (now < next) [[likely]] {
    return;
  }
  next = now + std::chrono::seconds(2);

  std::vector<std::string> names(256);
  std::vector<Gun> guns;
  std::string everything;  // for the log: every registered weapon, its inventory type and class
  for (uint32_t i = 1; i < 256; ++i) {
    names[i] = VariantName(base, i);
    if (names[i].empty()) {
      continue;
    }
    const uint32_t def = VariantDef(base, i);
    const uint32_t inventory = def ? Load32(base, def + kDefInventoryType) : 99;
    const uint32_t weapon_class = def ? Load32(base, def + kDefWeaponClass) : 99;
    everything += " " + names[i] + "/" + std::to_string(inventory) + "/" + std::to_string(weapon_class);
    if (inventory != kInventoryPrimary) {
      continue;
    }
    const GunCategory category = Category(names[i], weapon_class);
    if (category == kNotAGun) {
      continue;
    }
    const uint32_t variant = Load32(base, kWeaponVariants + i * 4);
    const uint32_t display = Load32(base, variant + kVariantDisplayName);
    std::string key = display >= 0x40000000u ? std::string(reinterpret_cast<const char*>(base + display),
                                                          strnlen(reinterpret_cast<const char*>(base + display), 64))
                                             : std::string();
    for (char& c : key) {
      if (!std::isalnum(uint8_t(c)) && c != '_') {
        c = '_';
      }
    }
    guns.push_back({names[i], key.empty() ? names[i] : key, category, false});
  }
  for (Gun& gun : guns) {
    gun.upgradable = !UpgradedName(base, names, gun.name).empty();
  }
  std::stable_sort(guns.begin(), guns.end(), [](const Gun& a, const Gun& b) { return a.category < b.category; });
  {
    std::lock_guard lock(g_catalog_mutex);
    g_map_weapons = names;
    g_map_guns.clear();
    for (const Gun& gun : guns) {
      g_map_guns.push_back(gun.name);
    }
  }

  // The map's perk machines, and the model number each shows (the vending trigger's target's word at + 0x16C, probably
  // a model index rather than a string id: read as a string id it gives unrelated tag and dvar names). It changes with
  // the machine's off and on models, so a test log shows POWER OFF and ON.
  std::string perks;
  std::string machines;
  for (uint32_t i = 1; i < kEntityCount; ++i) {
    const uint32_t entity = kGEntities + i * kEntitySize;
    if (base[entity + kEntInUse] == 0 || IdText(base, Load16(base, entity + kEntTargetName)) != kVendingName) {
      continue;
    }
    const std::string_view perk = IdText(base, Load16(base, entity + kEntNoteworthy));
    if (perk.substr(0, 10) != "specialty_") {
      continue;
    }
    const uint16_t target = Load16(base, entity + kEntTarget);
    for (uint32_t j = 1; target != 0 && j < kEntityCount; ++j) {
      const uint32_t machine = kGEntities + j * kEntitySize;
      if (base[machine + kEntInUse] != 0 && Load16(base, machine + kEntTargetName) == target) {
        machines += " " + std::string(perk.substr(10)) + ":" + std::to_string(Load16(base, machine + kEntModel));
        break;
      }
    }
    if (perk != "specialty_weapupgrade" && (" " + perks + " ").find(" " + std::string(perk) + " ") == std::string::npos) {
      perks += std::string(perks.empty() ? "" : " ") + std::string(perk);
    }
  }
  // And the perks the map turns on without a machine of their own (Origins' random perk machine sells PhD Flopper,
  // Deadshot, Electric Cherry...): ReadPerkFlags.
  {
    std::lock_guard lock(g_catalog_mutex);
    for (size_t at = 0; at < g_flag_perks.size();) {
      const size_t end = std::min(g_flag_perks.find(' ', at), g_flag_perks.size());
      const std::string perk = g_flag_perks.substr(at, end - at);
      if (!perk.empty() && (" " + perks + " ").find(" " + perk + " ") == std::string::npos) {
        perks += std::string(perks.empty() ? "" : " ") + perk;
      }
      at = end + 1;
    }
  }
  static std::string shown_machines;
  if (machines != shown_machines) {
    shown_machines = machines;
    REXLOG_INFO("[cheats] perk and Pack-a-Punch machines (perk:model number):{}", machines.empty() ? " none" : machines);
  }

  // The map's power-ups (the table above; empty for a map it does not know, and the menu then shows them all).
  std::string powerups;
  const std::string loaded = bo2zm::preset::LoadedMap();
  const std::string_view map = std::string_view(loaded).substr(0, loaded.find(' '));
  for (const auto& [name, list] : kMapPowerups) {
    if (name == map) {
      powerups = list;
    }
  }

  std::string guns_text;
  for (const Gun& gun : guns) {
    guns_text += gun.name + "," + gun.display + "," + kCategoryNames[gun.category] + "," +
                 (gun.upgradable ? "1" : "0") + " ";
  }
  std::string commands;
  if (guns_text != shown_guns) {
    shown_guns = guns_text;
    commands += "set bo2zm_guns_ui_count " + std::to_string(guns.size()) + "; ";
    std::string part;
    int parts = 0;
    for (size_t i = 0; i <= guns.size(); ++i) {
      const std::string entry = i < guns.size() ? guns[i].name + "," + guns[i].display + "," +
                                                      kCategoryNames[guns[i].category] + "," +
                                                      (guns[i].upgradable ? "1" : "0")
                                                : std::string();
      if (!part.empty() && (entry.empty() || part.size() + 1 + entry.size() > kDvarText)) {
        commands += "set bo2zm_guns_ui_" + std::to_string(parts++) + " \"" + part + "\"; ";
        part.clear();
      }
      part += std::string(part.empty() ? "" : " ") + entry;
    }
    REXLOG_INFO("[cheats] weapons: {} guns to give on this map: {}", guns.size(), guns_text);
  }
  if (!logged_weapons) {
    logged_weapons = true;
    REXLOG_INFO("[cheats] weapons registered (name/inventory type/class):{}", everything);
    LogZoneMemory(base, "in a match");
  }
  if (perks != shown_perks) {
    shown_perks = perks;
    commands += "set bo2zm_map_perks_ui \"" + perks + "\"; ";
    REXLOG_INFO("[cheats] perk machines on this map: {}", perks.empty() ? "none" : perks);
  }
  if (powerups != shown_powerups) {
    shown_powerups = powerups;
    commands += "set bo2zm_map_powerups_ui \"" + powerups + "\"; ";
    REXLOG_INFO("[cheats] power-ups on {}: {}", map.empty() ? "an unknown map" : map,
                powerups.empty() ? "not known (the menu shows them all)" : powerups);
  }
  {
    std::string text = "G" + guns_text + "\nP" + perks + "\nW" + powerups;
    std::lock_guard lock(g_catalog_mutex);
    if (text != g_catalog_text) {
      g_catalog_text = std::move(text);
      ++g_catalog_revision;
    }
  }
  if (!commands.empty()) {
    bo2zm::autoexec::ExecuteCommands(ctx, base, commands);
  }
}

// The menu's catalog dvars (see UpdateCatalog) for a joined match, from its host's catalog text. Main thread.
void ShowCatalog(PPCContext& ctx, uint8_t* base, std::string_view guns_text, std::string_view perks,
                 std::string_view powerups) {
  std::vector<std::string> guns;
  for (size_t at = 0; at < guns_text.size();) {
    const size_t end = std::min(guns_text.find(' ', at), guns_text.size());
    if (end > at) guns.emplace_back(guns_text.substr(at, end - at));
    at = end + 1;
  }
  std::string commands = "set bo2zm_guns_ui_count " + std::to_string(guns.size()) + "; ";
  std::string part;
  int parts = 0;
  for (size_t i = 0; i <= guns.size(); ++i) {
    const std::string entry = i < guns.size() ? guns[i] : std::string();
    if (!part.empty() && (entry.empty() || part.size() + 1 + entry.size() > kDvarText)) {
      commands += "set bo2zm_guns_ui_" + std::to_string(parts++) + " \"" + part + "\"; ";
      part.clear();
    }
    part += std::string(part.empty() ? "" : " ") + entry;
  }
  commands += "set bo2zm_map_perks_ui \"" + std::string(perks) + "\"; ";
  commands += "set bo2zm_map_powerups_ui \"" + std::string(powerups) + "\"; ";
  bo2zm::autoexec::ExecuteCommands(ctx, base, commands);
  REXLOG_INFO("[cheats] the host's catalog: {} guns, perks: {}, power-ups: {}", guns.size(),
              perks.empty() ? "none" : perks, powerups.empty() ? "all" : powerups);
}

// The registered weapon with this name (Server thread), 0 if none.
uint32_t MapWeapon(std::string_view name) {
  std::lock_guard lock(g_catalog_mutex);
  for (uint32_t i = 1; i < g_map_weapons.size(); ++i) {
    if (g_map_weapons[i] == name) {
      return i;
    }
  }
  return 0;
}

// The registered weapons' names (a copy), and whether a name is one of the menu's guns.
std::vector<std::string> MapWeaponNames() {
  std::lock_guard lock(g_catalog_mutex);
  return g_map_weapons;
}
bool MenuGun(std::string_view name) {
  std::lock_guard lock(g_catalog_mutex);
  return std::find(g_map_guns.begin(), g_map_guns.end(), name) != g_map_guns.end();
}

bool HasWeapon(PPCContext& ctx, uint8_t* base, uint32_t client, uint32_t weapon) {
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 256) & ~0xFu;
  ctx.r3.u64 = client;
  ctx.r4.u64 = weapon;
  bo2zm::guest::Call<kHasWeapon>(ctx, base);
  const bool has = ctx.r3.u32 != 0;
  ctx = saved;
  return has;
}

// Takes the gun in the selected player's hands the way the takeweapon method (sub_82313568) does: the weapon in hand and the
// client word at kClientLastWeapon cleared, then the player state's own take (sub_826B42B8: the inventory, the clip
// slots, a dual-wield gun's left hand). Only a gun (Category, its upgraded version too): not equipment, a perk bottle,
// the revive syrette, or hands such as Mob of the Dead's Afterlife (lightning_hands_zm, which counts as a primary).
// Returns the gun's name, or empty when nothing was taken.
std::string TakeGunInHand(PPCContext& ctx, uint8_t* base, uint32_t client) {
  const uint32_t weapon = Load32(base, client + kClientWeapon);
  if ((weapon & 0xFF) == 0 || (weapon & 0xFF) == 255 || !IsPrimary(base, weapon)) {
    return {};
  }
  const std::string name = VariantName(base, weapon & 0xFF);
  std::string base_name = name;
  if (const size_t upgraded = base_name.find("_upgraded"); upgraded != std::string::npos) {
    base_name.erase(upgraded, 9);
  }
  const uint32_t def = VariantDef(base, weapon);
  if (Category(base_name, def ? Load32(base, def + kDefWeaponClass) : 99) == kNotAGun) {
    return {};
  }
  Store32(base, client + kClientWeapon, 0);
  Store32(base, client + kClientLastWeapon, 0);
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 256) & ~0xFu;
  ctx.r3.u64 = client;
  ctx.r4.u64 = weapon;
  bo2zm::guest::Call<kTakeWeapon>(ctx, base);
  ctx = saved;
  return name;
}

// Gives the selected player a gun the game's way, _zm_weapons::weapon_give(weapon, is_upgrade) (the box's and the wall buys'
// give: the sound, the Pack-a-Punch camo for an upgraded gun, the ammo, the switch to it), into the place of the gun in
// their hands (taken first, unless they already have the one asked for: then weapon_give refills it and switches to
// it). pap: its Pack-a-Punch version, when the map has one.
void GiveGun(PPCContext& ctx, uint8_t* base, const Actor& actor, const std::string& name, bool pap) {
  const uint32_t client = actor.client;
  if (!MenuGun(name)) {
    REXLOG_WARN("[cheats] give {}: not one of this map's guns (the engine registers weapons only while the level "
                "starts, and only those whose files are loaded)", name);
    return;
  }
  std::string give = name;
  if (pap) {
    const std::string upgraded = UpgradedName(base, MapWeaponNames(), name);
    if (!upgraded.empty()) {
      give = upgraded;
    } else {
      REXLOG_INFO("[cheats] give {}: no Pack-a-Punch version on this map; the normal one", name);
    }
  }
  const uint32_t weapon = MapWeapon(give);
  if (!weapon) {
    REXLOG_WARN("[cheats] give {}: not registered any more", give);
    return;
  }
  std::string taken;
  if (!HasWeapon(ctx, base, client, weapon)) {
    taken = TakeGunInHand(ctx, base, client);
  }
  if (RunScript(ctx, base, kWeaponsScript, "weapon_give",
                {{ScriptArg::kString, give}, {ScriptArg::kInt, {}, {}, give != name ? 1 : 0}}, actor.entity)) {
    REXLOG_INFO("[cheats] give {}{}", give, taken.empty() ? "" : " in place of " + taken);
  }
}

// Pack-a-Punch the gun in the selected player's hands: its upgraded version in its place.
void PackGunInHand(PPCContext& ctx, uint8_t* base, const Actor& actor) {
  const uint32_t client = actor.client;
  const uint32_t weapon = Load32(base, client + kClientWeapon);
  const std::string name = (weapon & 0xFF) == 255 ? std::string() : VariantName(base, weapon & 0xFF);
  if (name.empty() || !IsPrimary(base, weapon)) {
    REXLOG_INFO("[cheats] Pack-a-Punch: no gun in hand");
    return;
  }
  if (name.find("_upgraded") != std::string::npos) {
    REXLOG_INFO("[cheats] Pack-a-Punch: {} is already upgraded", name);
    return;
  }
  const std::string upgraded = UpgradedName(base, MapWeaponNames(), name);
  if (upgraded.empty()) {
    REXLOG_INFO("[cheats] Pack-a-Punch: {} has no upgraded version on this map", name);
    return;
  }
  TakeGunInHand(ctx, base, client);
  if (RunScript(ctx, base, kWeaponsScript, "weapon_give", {{ScriptArg::kString, upgraded}, {ScriptArg::kInt, {}, {}, 1}},
                actor.entity)) {
    REXLOG_INFO("[cheats] Pack-a-Punch: {} -> {}", name, upgraded);
  }
}

// A script field the scripts set on an entity (self.<field>): the entity's script object from Scr_GetEntityId
// (sub_82536580: instance, entity number, class 0), then the variable lookup kScriptGetVariable (which never makes one)
// with the field's id as a loaded script has it linked. The value and its type byte (kScrInt, kScrString), or type 0
// when it is not there.
std::pair<uint32_t, uint8_t> EntityField(PPCContext& ctx, uint8_t* base, uint32_t entity, uint16_t field) {
  if (field == 0) {
    return {0, 0};
  }
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 256) & ~0xFu;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = (entity - kGEntities) / kEntitySize;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  bo2zm::guest::Call<kScriptEntityObject>(ctx, base);
  const uint32_t object = ctx.r3.u32 & 0xFFFF;
  ctx.r1.u64 = (saved.r1.u32 - 256) & ~0xFu;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = object;
  ctx.r5.u64 = field;
  bo2zm::guest::Call<kScriptGetVariable>(ctx, base);
  const uint32_t variable = ctx.r3.u32 & 0xFFFF;
  ctx = saved;
  const uint32_t values = Load32(base, kScrVarGlob + kScrValues);
  if (object == 0 || variable == 0 || values < 0x40000000u) {
    return {0, 0};
  }
  const uint32_t slot = values + variable * 16;
  return {Load32(base, slot), base[slot + 6]};
}

int32_t EntityIntField(PPCContext& ctx, uint8_t* base, uint32_t entity, uint16_t field, int32_t fallback) {
  const auto [value, type] = EntityField(ctx, base, entity, field);
  return type == kScrInt ? int32_t(value) : fallback;
}

std::string_view EntityStringField(PPCContext& ctx, uint8_t* base, uint32_t entity, uint16_t field) {
  const auto [value, type] = EntityField(ctx, base, entity, field);
  return type == kScrString ? IdText(base, uint16_t(value)) : std::string_view();
}

// The in-use entities with one of these targetnames (string ids compared by text).
std::vector<uint32_t> EntitiesNamed(const uint8_t* base, std::initializer_list<std::string_view> names) {
  std::vector<uint32_t> found;
  for (uint32_t i = 1; i < kEntityCount; ++i) {
    const uint32_t entity = kGEntities + i * kEntitySize;
    if (base[entity + kEntInUse] == 0) {
      continue;
    }
    const std::string_view name = IdText(base, Load16(base, entity + kEntTargetName));
    if (std::find(names.begin(), names.end(), name) != names.end()) {
      found.push_back(entity);
    }
  }
  return found;
}

// The entities a trigger's target names (a door's or a barricade's pieces).
std::vector<uint32_t> TargetedEntities(const uint8_t* base, uint32_t trigger) {
  std::vector<uint32_t> found;
  const uint16_t target = Load16(base, trigger + kEntTarget);
  for (uint32_t i = 1; target != 0 && i < kEntityCount; ++i) {
    const uint32_t entity = kGEntities + i * kEntitySize;
    if (base[entity + kEntInUse] != 0 && Load16(base, entity + kEntTargetName) == target) {
      found.push_back(entity);
    }
  }
  return found;
}

// Notifies an entity with no arguments (Scr_NotifyNum, see OpenDoors).
void NotifyEntity(PPCContext& ctx, uint8_t* base, uint32_t entity, uint16_t message) {
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 512) & ~0xFu;
  ctx.r3.u64 = (entity - kGEntities) / kEntitySize;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = message;
  ctx.r6.u64 = 0;
  bo2zm::guest::Call<kNotifyEntity>(ctx, base);
  ctx = saved;
}

// Doors. Each door trigger ("zombie_door") runs _zm_blockers::door_think, which waits for a purchase (door_buy; on Mob
// of the Dead, level._default_door_custom_logic, alcatraz_afterlife_doors, makes its "afterlife_door"s wait for an
// Afterlife shock instead) and then calls door_opened(cost). door_opened toggles the door: it opens it, or closes it
// again when it is open (the trigger's script field _door_open says which; door_activate moves, rotates or animates
// the door, so an animated one does not change its origin). So we end each trigger's door_think (its endon
// "kill_door_think") and run door_opened on it: OPEN on the closed doors, which skips the purchase whatever the map's
// own purchase logic, CLOSE on the open ones, and then door_think again so a closed door is bought (or shocked) the
// game's way again. CLOSE leaves the doors the power opens ("electric_door", "local_electric_door") alone: their
// door_think waits only for the power and would open them again at once. Debris ("zombie_debris") and airlocks run
// their own think (debris_think and the like), and OPEN buys them (see OpenDoors) by sending the forced purchase a
// player's would be: "trigger" with the selected player and force = 1, as _zm_blockers::quantum_bomb_open_nearest_door_result
// does, through Scr_NotifyNum (sub_8253CF78: entity number, class 0, string id, count; Scr_Notify, sub_8237CA70,
// calls it that way) after Scr_AddInt (sub_8253D758) and Scr_AddEntityNum (sub_8253D940: instance, number, class 0,
// and the word at 0x82976AC8, as the game passes it). Cleared debris is deleted by the game, so it cannot be put back.
struct DoorIds {
  uint16_t trigger = 0, kill = 0, open = 0, cost = 0;
};
// Debris bought by OpenDoors, settled on a later server frame (SettleDebris): Server thread only.
struct DebrisPurchase {
  struct Pile {
    uint32_t trigger = 0;
    uint16_t name = 0;  // the trigger's targetname id, to tell it from a new entity in the same slot
    int32_t cost = 0;
  };
  Actor purchaser;
  std::vector<Pile> piles;
  uint32_t lent = 0;
  int frames = 0;
};
DebrisPurchase g_debris_purchase;

DoorIds LinkDoorIds(PPCContext& ctx, uint8_t* base) {
  return {LinkedId(ctx, base, kBlockersScript, "trigger", kFixupString),
          LinkedId(ctx, base, kBlockersScript, "kill_door_think", kFixupString),
          LinkedId(ctx, base, kBlockersScript, "_door_open", kFixupFieldName),
          LinkedId(ctx, base, kBlockersScript, "zombie_cost", kFixupFieldName)};
}

bool IsPowerDoor(const uint8_t* base, uint32_t trigger) {
  return IdText(base, Load16(base, trigger + kEntNoteworthy)).find("electric") != std::string_view::npos;
}

// door_opened(cost) on a trigger (the cost only goes into the hint a closed door shows: the door's own, or door_think's
// 1000).
void ToggleDoor(PPCContext& ctx, uint8_t* base, const DoorIds& ids, uint32_t trigger) {
  const int32_t cost = EntityIntField(ctx, base, trigger, ids.cost, 1000);
  RunScript(ctx, base, kBlockersScript, "door_opened", {{ScriptArg::kInt, {}, {}, cost}}, trigger);
}

void OpenDoors(PPCContext& ctx, uint8_t* base, const Actor& actor) {
  const DoorIds ids = LinkDoorIds(ctx, base);
  if (!ids.trigger || !ids.kill || !ids.open) {
    REXLOG_WARN("[cheats] open doors: {} has no linked \"trigger\", \"kill_door_think\" or _door_open", kBlockersScript);
    return;
  }
  const ServerClientFields server(ctx, base);
  const std::vector<uint32_t> doors = EntitiesNamed(base, {"zombie_door"});
  int was_open = 0;
  std::string kinds;
  for (const uint32_t door : doors) {
    if (EntityIntField(ctx, base, door, ids.open, 0) == 1) {
      ++was_open;
    } else {
      NotifyEntity(ctx, base, door, ids.kill);
    }
  }
  int opened = 0;
  for (const uint32_t door : doors) {
    if (EntityIntField(ctx, base, door, ids.open, 0) != 1) {  // a door's other triggers are opened with it
      ToggleDoor(ctx, base, ids, door);
      ++opened;
      const std::string_view kind = IdText(base, Load16(base, door + kEntNoteworthy));
      kinds += " " + std::string(kind.empty() ? "default" : kind);
    }
  }
  int still_closed = 0;
  for (const uint32_t door : doors) {
    still_closed += EntityIntField(ctx, base, door, ids.open, 0) != 1;
  }
  REXLOG_INFO("[cheats] open doors: {} door triggers ({} open before), door_opened on {} (kinds:{}), {} still closed "
              "after", doors.size(), was_open, opened, kinds, still_closed);
  // Debris and airlocks are bought: "trigger" with force 1 and player 1, pushed in that order because the last value
  // pushed becomes the waittill's first variable. With force, debris_think skips the use button but still takes
  // zombie_cost from the score and refuses a player who has less; and a notify only queues the waiting thread, which
  // runs in the next server frame. So player 1 is lent the piles' costs now, and SettleDebris takes the loan back after
  // that frame: the clearing costs nothing. Only one trigger per pile (the triggers that share its target) is sent the
  // purchase, since the purchase deletes them all.
  if (g_debris_purchase.frames != 0) {
    return;  // the last purchase is not settled yet
  }
  const uint32_t client = actor.client;
  DebrisPurchase purchase;
  purchase.purchaser = actor;
  std::vector<uint16_t> piles_seen;
  for (const uint32_t trigger : EntitiesNamed(base, {"zombie_debris", "zombie_airlock_buy"})) {
    const uint16_t target = Load16(base, trigger + kEntTarget);
    if (target != 0 && std::find(piles_seen.begin(), piles_seen.end(), target) != piles_seen.end()) {
      continue;
    }
    piles_seen.push_back(target);
    const int32_t cost = std::max(EntityIntField(ctx, base, trigger, ids.cost, 1000), 0);
    purchase.piles.push_back({trigger, Load16(base, trigger + kEntTargetName), cost});
    purchase.lent += uint32_t(cost);
  }
  if (!client || purchase.piles.empty()) {
    return;
  }
  Store32(base, client + kClientScore, Load32(base, client + kClientScore) + purchase.lent);
  const PPCContext saved = ctx;
  const uint32_t sp = (ctx.r1.u32 - 512) & ~0xFu;
  for (const DebrisPurchase::Pile& pile : purchase.piles) {
    ctx.r1.u64 = sp;
    ctx.r3.u64 = 0;
    ctx.r4.u64 = 1;  // force
    bo2zm::guest::Call<kScriptPushInt>(ctx, base);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = 0;
    ctx.r4.u64 = purchase.purchaser.number;  // the captured purchaser
    ctx.r5.u64 = 0;
    ctx.r6.u64 = Load32(base, kScriptEntityContext);
    bo2zm::guest::Call<kScriptPushEntity>(ctx, base);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = (pile.trigger - kGEntities) / kEntitySize;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = ids.trigger;
    ctx.r6.u64 = 2;
    bo2zm::guest::Call<kNotifyEntity>(ctx, base);
  }
  ctx = saved;
  purchase.frames = 1;
  g_debris_purchase = std::move(purchase);
}

// After OpenDoors' debris purchases: once the piles' triggers are gone (or a few frames on), takes back what was lent
// and not spent, and gives back what was spent. Server thread, after each server frame.
void SettleDebris(uint8_t* base) {
  DebrisPurchase& purchase = g_debris_purchase;
  if (purchase.frames == 0) [[likely]] {
    return;
  }
  const uint32_t client = CurrentActor(base, purchase.purchaser) ? purchase.purchaser.client : 0;
  if (!client) {
    purchase = {};
    return;
  }
  uint32_t spent = 0;
  size_t left = 0;
  for (const DebrisPurchase::Pile& pile : purchase.piles) {
    if (base[pile.trigger + kEntInUse] != 0 && Load16(base, pile.trigger + kEntTargetName) == pile.name) {
      ++left;
    } else {
      spent += uint32_t(pile.cost);
    }
  }
  if (left != 0 && purchase.frames++ < 4) {
    return;
  }
  const uint32_t score = Load32(base, client + kClientScore);
  const uint32_t settled = score + spent > purchase.lent ? score + spent - purchase.lent : 0;
  Store32(base, client + kClientScore, settled);
  REXLOG_INFO("[cheats] open doors: {} debris piles sent the purchase, {} bought within {} server frames ({} points, "
              "given back), {} left; score {}", purchase.piles.size(), purchase.piles.size() - left, purchase.frames,
              spent, left, settled);
  purchase = {};
}

void CloseDoors(PPCContext& ctx, uint8_t* base) {
  const DoorIds ids = LinkDoorIds(ctx, base);
  if (!ids.kill || !ids.open) {
    REXLOG_WARN("[cheats] close doors: {} has no linked \"kill_door_think\" or _door_open", kBlockersScript);
    return;
  }
  const ServerClientFields server(ctx, base);
  std::vector<uint32_t> open;
  int power_doors = 0;
  for (const uint32_t door : EntitiesNamed(base, {"zombie_door"})) {
    if (EntityIntField(ctx, base, door, ids.open, 0) != 1) {
      continue;
    }
    if (IsPowerDoor(base, door)) {
      ++power_doors;
      continue;
    }
    open.push_back(door);
    NotifyEntity(ctx, base, door, ids.kill);
  }
  std::string costs;
  for (const uint32_t door : open) {
    if (EntityIntField(ctx, base, door, ids.open, 0) == 1) {  // a door's other triggers close with it
      ToggleDoor(ctx, base, ids, door);
      costs += " " + std::to_string(EntityIntField(ctx, base, door, ids.cost, 1000));
    }
  }
  int still_open = 0;
  for (const uint32_t door : open) {
    still_open += EntityIntField(ctx, base, door, ids.open, 0) == 1;
    RunScript(ctx, base, kBlockersScript, "door_think", {}, door);
  }
  REXLOG_INFO("[cheats] close doors: {} open door triggers closed (costs{}), {} still open after, door_think again on "
              "each; {} open power doors left open", open.size(), costs, still_open, power_doors);
}

// Runs a function at handle with a script object (a struct) as self: what the entity thread starter sub_82543248 does
// once Scr_GetEntityId has given it the entity's object. The VM's per-instance block at kScrVmPub: a first call stores
// sub_824E2458's value at kScrVmStart; the object gets a reference (the u16 at objects + id * 8); sub_82535128
// (instance, object) makes the thread, sub_82542F00 (instance, thread, function, count) runs it; then the returned
// value at the stack top is released (sub_82206298: instance, its two words), the top goes down 8 bytes and the
// in-parameter count by one. The thread (for kScriptFreeThread).
uint32_t RunObjectThread(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t handle, uint32_t count,
                         uint32_t sp) {
  constexpr uint32_t kScrVmPub = 0x840DF138;  // 17064 bytes per instance
  constexpr uint32_t kVmCalls = 8, kVmTop = 16, kVmInParams = 24;
  constexpr uint32_t kScrVmStart = 0x840FD1A0;
  constexpr uint32_t kVmStartValue = 0x824E2458;  // call only: the VM's first-call value
  constexpr uint32_t kAllocThread = 0x82535128;   // call only: a thread for an object
  constexpr uint32_t kExecute = 0x82542F00;       // call only: runs a thread
  constexpr uint32_t kReleaseValue = 0x82206298;  // call only: releases a script value
  if (Load32(base, kScrVmPub + kVmCalls) == 0) {
    ctx.r1.u64 = sp;
    bo2zm::guest::Call<kVmStartValue>(ctx, base);
    Store32(base, kScrVmStart, ctx.r3.u32);
  }
  const uint32_t objects = Load32(base, kScrVarGlob + kScrObjects);
  const uint16_t refs = uint16_t(Load16(base, objects + object * 8) + 1);
  base[objects + object * 8] = uint8_t(refs >> 8);
  base[objects + object * 8 + 1] = uint8_t(refs);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = object;
  bo2zm::guest::Call<kAllocThread>(ctx, base);
  const uint32_t thread = ctx.r3.u32;
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = thread;
  ctx.r5.u64 = handle;
  ctx.r6.u64 = count;
  bo2zm::guest::Call<kExecute>(ctx, base);
  const uint32_t result = ctx.r3.u32;
  const uint32_t top = Load32(base, kScrVmPub + kVmTop);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = Load32(base, top);
  ctx.r5.u64 = Load32(base, top + 4);
  bo2zm::guest::Call<kReleaseValue>(ctx, base);
  Store32(base, top, 0);
  Store32(base, kScrVmPub + kVmTop, top - 8);
  Store32(base, kScrVmPub + kVmInParams, Load32(base, kScrVmPub + kVmInParams) - 1);
  return result & 0xFFFF;
}

// A child variable of a script object or array by name (a field's or a string key's id, or an array index + 0x800000:
// the key Scr_GetEntityId, sub_82536580, builds for its entity arrays), through the find-only lookup: its value and
// type byte, or type 0 when there is none.
std::pair<uint32_t, uint8_t> ChildValue(PPCContext& ctx, uint8_t* base, uint32_t parent, uint32_t name) {
  const uint32_t values = Load32(base, kScrVarGlob + kScrValues);
  if (parent == 0 || name == 0 || values < 0x40000000u) {
    return {0, 0};
  }
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 256) & ~0xFu;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = parent;
  ctx.r5.u64 = name;
  bo2zm::guest::Call<kScriptGetVariable>(ctx, base);
  const uint32_t variable = ctx.r3.u32 & 0xFFFF;
  ctx = saved;
  if (variable == 0) {
    return {0, 0};
  }
  const uint32_t slot = values + variable * 16;
  return {Load32(base, slot), base[slot + 6]};
}

// The script structs getstructarray(value, key) returns: common_scripts/utility::getstructarray reads
// level.struct_class_names[key][value], an array of the structs. Their object ids.
std::vector<uint32_t> StructArray(PPCContext& ctx, uint8_t* base, std::string_view key, std::string_view value,
                                  std::string_view linked_in) {
  std::vector<uint32_t> found;
  const uint32_t level = Load32(base, kScrVarPub + kScrLevelObject);
  const auto [classes, classes_type] =
      ChildValue(ctx, base, level, LinkedId(ctx, base, kUtilityScript, "struct_class_names", kFixupFieldName));
  const auto [by_key, by_key_type] =
      ChildValue(ctx, base, classes & 0xFFFF, LinkedId(ctx, base, linked_in, key, kFixupString));
  const auto [array, array_type] =
      ChildValue(ctx, base, by_key & 0xFFFF, LinkedId(ctx, base, linked_in, value, kFixupString));
  for (uint32_t i = 0; array_type != 0 && i < 64; ++i) {
    const auto [object, type] = ChildValue(ctx, base, array & 0xFFFF, i + 0x800000);
    if (type == 0) {
      break;
    }
    found.push_back(object & 0xFFFF);
  }
  REXLOG_INFO("[cheats] structs {} \"{}\": level.struct_class_names type {}, [\"{}\"] type {}, [\"{}\"] type {}, {} "
              "structs", key, value, classes_type, key, by_key_type, value, array_type, found.size());
  return found;
}

// Origins' generators: the script structs with targetname "s_generator", each a capture zone
// of maps/mp/zm_tomb_capture_zones.gsc. Capturing one is set_player_controlled_area() on its struct (what
// players_capture_zone runs when a capture completes: set_player_controlled_zone sets its "player_controlled" flag,
// the HUD and crystal client fields, and turns on the perk machines, random perk machine and mystery box of its zone),
// with generator_state_power_up() for the generator's own state client field; losing one is
// set_zombie_controlled_area(0) and generator_state_turn_off(). The developer-only debug_set_generator_active does the
// same pair. Returns the generators found.
int SetGenerators(PPCContext& ctx, uint8_t* base, bool on) {
  constexpr const char* kCaptureScript = "maps/mp/zm_tomb_capture_zones";
  if (!LinkedId(ctx, base, kCaptureScript, "s_generator", kFixupString)) {
    return 0;  // not Origins
  }
  const std::vector<uint32_t> structs = StructArray(ctx, base, "targetname", "s_generator", kCaptureScript);
  for (const uint32_t s : structs) {
    if (on) {
      RunScript(ctx, base, kCaptureScript, "set_player_controlled_area", {}, 0, s);
      RunScript(ctx, base, kCaptureScript, "generator_state_power_up", {}, 0, s);
    } else {
      RunScript(ctx, base, kCaptureScript, "set_zombie_controlled_area", {{ScriptArg::kInt, {}, {}, 0}}, 0, s);
      RunScript(ctx, base, kCaptureScript, "generator_state_turn_off", {}, 0, s);
    }
  }
  REXLOG_INFO("[cheats] power {}: {} Origins generators {}", on ? "on" : "off", structs.size(),
              on ? "captured" : "lost to the zombies");
  return int(structs.size());
}

// Power on: _zm_game_module::turn_power_on_and_open_doors (the game's own for Grief and Turned): the "power_on" flag,
// which _zm_power::watch_global_power waits for to power every machine, Pack-a-Punch and electric door; the client
// field zombie_power_on (the lights) and the electric doors' own notifies. Power off: common_scripts/utility::
// flag_clear("power_on"), which watch_global_power waits for to turn them off again (the machines stop selling, the
// lights stay as they were: the client field is not cleared). On Origins also every generator (SetGenerators).
void SetPower(PPCContext& ctx, uint8_t* base, bool on) {
  const uint32_t thread = on ? RunScript(ctx, base, kGameModuleScript, "turn_power_on_and_open_doors", {})
                             : RunScript(ctx, base, kUtilityScript, "flag_clear", {{ScriptArg::kString, "power_on"}});
  if (thread) {
    REXLOG_INFO("[cheats] power {}", on ? "on" : "off");
  }
  SetGenerators(ctx, base, on);
}

// Free debris: what clears without a purchase. Buried's barricades that Leroy breaks are the
// triggers with targetname "sloth_barricade" (_zm_ai_sloth::init_barricades; watch_barricade breaks one only for
// Leroy in a rage): REMOVE sets the trigger's script_flag (the zones behind it, as watch_barricade and Grief's
// zm_buried_gamemodes::deleteslothbarricades do) and hides its pieces with _zm_ai_sloth::hide_sloth_barrier (not
// solid, ghosted, paths connected for a clip: what a "no_delete" barricade does when broken); PUT BACK runs
// unhide_sloth_barrier on them (solid, shown, paths cut again). The zone flags stay set. A barricade Leroy already
// broke is deleted: nothing to put back.
void SetDebris(PPCContext& ctx, uint8_t* base, bool remove) {
  constexpr const char* kSlothScript = "maps/mp/zombies/_zm_ai_sloth";
  const std::vector<uint32_t> barricades = EntitiesNamed(base, {"sloth_barricade"});
  if (barricades.empty()) {
    REXLOG_INFO("[cheats] debris: no free debris on this map (no sloth_barricade)");
    return;
  }
  const uint16_t flag_field = LinkedId(ctx, base, kSlothScript, "script_flag", kFixupFieldName);
  const ServerClientFields server(ctx, base);
  int pieces = 0;
  std::string flags;
  for (const uint32_t barricade : barricades) {
    if (remove) {
      const std::string flag(EntityStringField(ctx, base, barricade, flag_field));
      if (!flag.empty()) {
        RunScript(ctx, base, kUtilityScript, "flag_set", {{ScriptArg::kString, flag}});
        flags += " " + flag;
      }
    }
    for (const uint32_t piece : TargetedEntities(base, barricade)) {
      if (RunScript(ctx, base, kSlothScript, remove ? "hide_sloth_barrier" : "unhide_sloth_barrier", {}, piece)) {
        ++pieces;
      }
    }
  }
  // The pieces' own is_hidden field, which the two functions set, as evidence.
  const uint16_t hidden_field = LinkedId(ctx, base, kSlothScript, "is_hidden", kFixupFieldName);
  int hidden = 0;
  for (const uint32_t barricade : barricades) {
    for (const uint32_t piece : TargetedEntities(base, barricade)) {
      hidden += EntityIntField(ctx, base, piece, hidden_field, 0) == 1;
    }
  }
  REXLOG_INFO("[cheats] debris {}: {} Leroy barricades, {} pieces, {} hidden now{}", remove ? "removed" : "put back",
              barricades.size(), pieces, hidden, remove ? ", zone flags set:" + flags : std::string());
}

// The perks the map turns on: each map's main script sets level.zombiemode_using_<perk>_perk (to 1) for its perks,
// and _zm_perks::perks_register_clientfield registers their HUD fields from them; Origins sets nine and starts its
// random perk machine (maps/mp/zm_tomb.gsc). The field's id as _zm_perks.gsc or the map's
// script has it linked (a perk only its own script names, PhD Flopper or Electric Cherry, is in the map's), the value
// through LevelIntSlot. Server thread, every 2 s in a match.
void ReadPerkFlags(PPCContext& ctx, uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  constexpr std::pair<const char*, const char*> kPerkFlags[] = {
      {"specialty_armorvest", "zombiemode_using_juggernaut_perk"},
      {"specialty_quickrevive", "zombiemode_using_revive_perk"},
      {"specialty_fastreload", "zombiemode_using_sleightofhand_perk"},
      {"specialty_rof", "zombiemode_using_doubletap_perk"},
      {"specialty_longersprint", "zombiemode_using_marathon_perk"},
      {"specialty_deadshot", "zombiemode_using_deadshot_perk"},
      {"specialty_additionalprimaryweapon", "zombiemode_using_additionalprimaryweapon_perk"},
      {"specialty_scavenger", "zombiemode_using_tombstone_perk"},
      {"specialty_finalstand", "zombiemode_using_chugabud_perk"},
      {"specialty_flakjacket", "zombiemode_using_divetonuke_perk"},
      {"specialty_grenadepulldeath", "zombiemode_using_electric_cherry_perk"}};
  const auto now = Clock::now();
  if (now < next) [[likely]] {
    return;
  }
  next = now + std::chrono::seconds(2);
  const std::string loaded = bo2zm::preset::LoadedMap();
  const std::string map_script = "maps/mp/" + loaded.substr(0, loaded.find(' '));
  std::string perks;
  for (const auto& [perk, flag] : kPerkFlags) {
    uint16_t field = LinkedId(ctx, base, kPerksScript, flag, kFixupFieldName);
    if (!field) {
      field = LinkedId(ctx, base, map_script, flag, kFixupFieldName);
    }
    const uint32_t slot = LevelIntSlot(ctx, base, field);
    if (slot && Load32(base, slot) != 0) {
      perks += std::string(perks.empty() ? "" : " ") + perk;
    }
  }
  std::lock_guard lock(g_catalog_mutex);
  if (perks != g_flag_perks) {
    g_flag_perks = perks;
    REXLOG_INFO("[cheats] perks the map turns on (level.zombiemode_using_*_perk): {}", perks.empty() ? "none" : perks);
  }
}

// Test runs (bo2zm_test_cheats): each "seconds:commands" entry runs its console commands that many seconds after
// the selected player is first in a match. Main thread.
void RunTestCommands(PPCContext& ctx, uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  static std::vector<std::pair<int, std::string>> steps;
  static bool parsed = false;
  static Clock::time_point start{};
  static size_t next = 0;
  if (!parsed) {
    parsed = true;
    std::string_view text = REXCVAR_GET(bo2zm_test_cheats);
    while (!text.empty()) {
      const size_t bar = text.find('|');
      const std::string_view entry = text.substr(0, bar);
      if (const size_t colon = entry.find(':'); colon != std::string_view::npos) {
        steps.emplace_back(std::atoi(std::string(entry.substr(0, colon)).c_str()), std::string(entry.substr(colon + 1)));
      }
      text = bar == std::string_view::npos ? std::string_view() : text.substr(bar + 1);
    }
  }
  if (next >= steps.size()) [[likely]] {
    return;
  }
  if (!Client(base)) {
    return;
  }
  if (start == Clock::time_point{}) {
    start = Clock::now();
  }
  while (next < steps.size() && Clock::now() - start >= std::chrono::seconds(steps[next].first)) {
    REXLOG_INFO("[cheats] test step at {} s: {}", steps[next].first, steps[next].second);
    bo2zm::autoexec::ExecuteCommands(ctx, base, steps[next].second);
    ++next;
  }
}

// The menu's gun, Pack-a-Punch, door and power requests: Server thread, between server frames (ServerFrame).
void RunWorldRequests(PPCContext& ctx, uint8_t* base) {
  if (!g_world_waiting.load(std::memory_order_relaxed)) return;
  int doors = 0, power = 0, debris = 0;
  Actor actor;
  {
    std::lock_guard lock(g_script_mutex);
    g_world_waiting.store(false, std::memory_order_relaxed);
    std::swap(doors, g_doors_request);
    std::swap(power, g_power_request);
    std::swap(debris, g_debris_request);
    actor = g_world_target;
  }
  if (!CurrentActor(base, actor) || !actor.client ||
      (actor.remote < 0 && actor.local != bo2zm::cheats::PrimaryLocalPlayer()) ||
      !StateForActor(actor).options.Effective(Option::Cheats)) return;
  if (power != 0) SetPower(ctx, base, power == 1);
  if (doors == 1) OpenDoors(ctx, base, actor);
  else if (doors == 2) CloseDoors(ctx, base);
  if (debris != 0) SetDebris(ctx, base, debris == 1);
}

}  // namespace

bool bo2zm::cheats::GodMode() {
  return PrimaryState().options.Effective(Option::God);
}
void bo2zm::cheats::SetGodMode(bool on) {
  LegacyOption(Option::God, on);
}
int bo2zm::cheats::FlyMode() {
  return PrimaryState().options.Effective(Option::Fly);
}
void bo2zm::cheats::SetFlyMode(int mode) {
  LegacyOption(Option::Fly, mode == kNoclip || mode == kUfo ? mode : kFlyOff);
}
bool bo2zm::cheats::FlyCollision() {
  return PrimaryState().options.Effective(Option::Collision);
}
void bo2zm::cheats::SetFlyCollision(bool on) {
  LegacyOption(Option::Collision, on);
}
bool bo2zm::cheats::Combo() {
  return PrimaryState().options.Effective(Option::Combo);
}
void bo2zm::cheats::SetCombo(bool on) {
  LegacyOption(Option::Combo, on);
}
int bo2zm::cheats::ComboMode() {
  return PrimaryState().options.Effective(Option::ComboMode) + 1;
}
void bo2zm::cheats::SetComboMode(int mode) {
  LegacyOption(Option::ComboMode, mode == kUfo ? 1 : 0);
}
int bo2zm::cheats::JumpLevel() {
  return PrimaryState().options.Effective(Option::Jump);
}
void bo2zm::cheats::SetJumpLevel(int level) {
  LegacyOption(Option::Jump, level);
}
bool bo2zm::cheats::HudHidden() {
  return bo2zm::mode::IsZombies() && PrimaryState().options.Effective(Option::Hud);
}
void bo2zm::cheats::SetHudHidden(bool hidden) {
  LegacyOption(Option::Hud, hidden);
}
bool bo2zm::cheats::ThirdPerson() {
  return PrimaryState().options.Effective(Option::Third);
}
void bo2zm::cheats::SetThirdPerson(bool on) {
  LegacyOption(Option::Third, on);
}
int bo2zm::cheats::ThirdPersonCrosshairSetting() {
  return PrimaryState().options.Effective(Option::ThirdCrosshair);
}
void bo2zm::cheats::SetThirdPersonCrosshair(int mode) {
  LegacyOption(Option::ThirdCrosshair, mode);
}
int bo2zm::cheats::CrosshairType() {
  return PrimaryState().options.Effective(Option::CrosshairType);
}
void bo2zm::cheats::SetCrosshairType(int type) {
  LegacyOption(Option::CrosshairType, type);
}
int bo2zm::cheats::CrosshairColor() {
  return PrimaryState().options.Effective(Option::CrosshairColor);
}
void bo2zm::cheats::SetCrosshairColor(int color) {
  LegacyOption(Option::CrosshairColor, color);
}
bool bo2zm::cheats::ThirdPersonCrosshair() {
  const auto& state = PrimaryState();
  if (!bo2zm::mode::IsZombies() || !state.options.Effective(Option::Third) ||
      !state.in_match.load(std::memory_order_relaxed)) {
    return false;
  }
  const int mode = state.options.Effective(Option::ThirdCrosshair);
  return mode == 2 || (mode == 1 && state.aiming.load(std::memory_order_relaxed));
}
bool bo2zm::cheats::ExplosiveBullets() {
  return PrimaryState().options.Effective(Option::Explosive);
}
void bo2zm::cheats::SetExplosiveBullets(bool on) {
  LegacyOption(Option::Explosive, on);
}
bool bo2zm::cheats::KeepPerksOn() {
  return PrimaryState().options.Effective(Option::KeepPerks);
}
void bo2zm::cheats::SetKeepPerks(bool on) {
  LegacyOption(Option::KeepPerks, on);
}
bool bo2zm::cheats::InMatch() {
  // A joined System Link game has no local players on this console's server: the client says it is in one.
  return !g_map_loading.load(std::memory_order_acquire) &&
         (g_in_match.load(std::memory_order_relaxed) || bo2zm::preset::ClientInGame());
}
bool bo2zm::cheats::GivePackAPunch() {
  return PrimaryState().options.Effective(Option::GivePap);
}
void bo2zm::cheats::SetGivePackAPunch(bool on) {
  LegacyOption(Option::GivePap, on);
}
bool bo2zm::cheats::CrossMapPerks() {
  return PrimaryState().options.Effective(Option::CrossPerks);
}
void bo2zm::cheats::SetCrossMapPerks(bool on) {
  LegacyOption(Option::CrossPerks, on);
}
int bo2zm::cheats::CurrentRound() {
  return g_round_shown.load(std::memory_order_relaxed);
}

namespace {
std::mutex g_spot_mutex;
bo2zm::cheats::Spot g_spot{};  // under g_spot_mutex
bool g_spot_valid = false;     // under g_spot_mutex

// Player 0's place for PlayerSpot, once a frame (main thread).
void TakeSpot(const uint8_t* base) {
  const Actor actor = PrimaryActor();
  const uint32_t client = actor.client;
  std::lock_guard lock(g_spot_mutex);
  g_spot_valid = client != 0;
  if (!client) {
    return;
  }
  const Vec3 feet = LoadVec(base, actor.entity + kEntOrigin);
  const Vec3 camera = actor.cg ? LoadVec(base, actor.cg + kCgViewOrigin) : feet;
  const Vec3 angles = LoadVec(base, client + kPsViewAngles);
  g_spot = {{feet.x, feet.y, feet.z}, {camera.x, camera.y, camera.z}, {angles.x, angles.y, angles.z}};
}
}  // namespace

bool bo2zm::cheats::PlayerSpot(Spot& out) {
  std::lock_guard lock(g_spot_mutex);
  out = g_spot;
  return g_spot_valid && !g_map_loading.load(std::memory_order_acquire);
}
void bo2zm::cheats::GoToRound(int round) {
  if (!bo2zm::mode::IsZombies()) {
    return;
  }
  if (g_joined.load(std::memory_order_acquire)) {
    bo2zm::cheat_sync::Forward(g_primary_controller.load(std::memory_order_relaxed), "bo2zm_goto_round",
                               std::to_string(std::clamp(round, 1, 255)), false);
    return;
  }
  const Actor actor = PrimaryActor();
  if (!actor || g_map_loading.load(std::memory_order_acquire)) return;
  std::lock_guard lock(g_script_mutex);
  if (actor.epoch != g_map_epoch.load(std::memory_order_acquire) || g_map_loading.load(std::memory_order_acquire)) return;
  g_round_target = actor;
  g_round_request = std::clamp(round, 1, 255);
  g_script_waiting.store(true, std::memory_order_relaxed);
}
bool bo2zm::cheats::ZombiesIgnore() {
  return PrimaryState().options.Effective(Option::Ignore);
}
void bo2zm::cheats::SetZombiesIgnore(bool on) {
  LegacyOption(Option::Ignore, on);
}
bool bo2zm::cheats::StopSpawns() {
  return g_stop_spawns.load(std::memory_order_relaxed);
}
void bo2zm::cheats::SetStopSpawns(bool on) {
  if (g_joined.load(std::memory_order_acquire)) {
    // The host's setting; this console shows it once the host has it (ApplyWorldState).
    bo2zm::cheat_sync::Forward(g_primary_controller.load(std::memory_order_relaxed), "bo2zm_zm_nospawn", on ? "1" : "0",
                               false);
    return;
  }
  g_stop_spawns.store(on, std::memory_order_relaxed);
}
int bo2zm::cheats::MoveSpeed() {
  return PrimaryState().options.Effective(Option::Speed);
}
void bo2zm::cheats::SetMoveSpeed(int level) {
  LegacyOption(Option::Speed, level);
}
bool bo2zm::cheats::NoRecoil() {
  return PrimaryState().options.Effective(Option::NoRecoil);
}
void bo2zm::cheats::SetNoRecoil(bool on) {
  LegacyOption(Option::NoRecoil, on);
}
bool bo2zm::cheats::InfiniteAmmo() {
  return PrimaryState().options.Effective(Option::Ammo);
}
void bo2zm::cheats::SetInfiniteAmmo(bool on) {
  LegacyOption(Option::Ammo, on);
}
int bo2zm::cheats::FieldOfView() {
  return PrimaryState().options.Effective(Option::Fov);
}
void bo2zm::cheats::SetFieldOfView(int level) {
  LegacyOption(Option::Fov, level);
}
bool bo2zm::cheats::FeatherFall() {
  return PrimaryState().options.Effective(Option::Feather);
}
void bo2zm::cheats::SetFeatherFall(bool on) {
  LegacyOption(Option::Feather, on);
}

template<class Pad>
static uint16_t PlayerPadForController(const Pad& pad, int local, uint32_t controller) {
  if (!bo2zm::mode::IsZombies() || local < 0 || local >= kLocalPlayers || controller >= kLocalPlayers) return 0;
  std::lock_guard lock(g_actors_mutex);
  if (g_controllers[local] != int(controller)) return 0;
  const Actor& actor = g_actors[local];
  if (!actor || actor.controller != int(controller) || actor.epoch != g_map_epoch.load(std::memory_order_acquire) ||
      g_map_loading.load(std::memory_order_acquire)) return 0;
  auto& state = g_players[controller];
  state.aiming.store(pad.aim, std::memory_order_relaxed);
  return state.options.QueueCombo(pad.aim, pad.Pressed(bo2zm::pad::kDown), actor.generation) ? bo2zm::pad::kDown : 0;
}

template<class Pad>
static uint16_t ScopedPlayerPad(const Pad& pad, int local) {
  if constexpr (requires { pad.user; }) return PlayerPadForController(pad, local, pad.user);
  else return 0;  // Scoped input requires the paired bridge's explicit controller tag.
}

uint16_t bo2zm::cheats::OnPad(const bo2zm::pad::Pad& pad) {
  return PlayerPadForController(pad, PrimaryLocalPlayer(), uint32_t(g_primary_controller.load(std::memory_order_relaxed)));
}

uint16_t bo2zm::cheats::OnPad(const bo2zm::pad::Pad& pad, int local) {
  return ScopedPlayerPad(pad, local);
}

namespace {

// The menu's cheat commands that CHEATS off refuses (bo2zm_release and bo2zm_reset_cheats always run).
bool MenuCheatCommand(std::string_view name) {
  return name == "bo2zm_perk" || name == "bo2zm_remove_perks" || name == "bo2zm_powerup" ||
         name == "bo2zm_kill_zombies" || name == "bo2zm_max_points" || name == "bo2zm_hold" ||
         name == "bo2zm_give_weapon" || name == "bo2zm_pap_weapon" || name == "bo2zm_doors" || name == "bo2zm_power" ||
         name == "bo2zm_debris";
}
}  // namespace

int bo2zm::cheats::MenuCheats() {
  return bo2zm::mode::IsZombies() ? PrimaryState().options.Effective(Option::Cheats) : 0;
}

namespace {
// The cheat settings back to normal play (the held dvars are the caller's).
void CheatsToNormal() {
  PrimaryState().options.ResetGameplay();
  g_stop_spawns.store(false, std::memory_order_relaxed);
}
}  // namespace

static void SetPlayerMaster(int local, int on, bool legacy = false) {
  auto& state = StateForLocal(local);
  const int was = state.options.Effective(Option::Cheats);
  state.options.Set("cheats", on);
  if (was == (on ? 1 : 0)) {
    return;
  }
  const int primary = bo2zm::cheats::PrimaryLocalPlayer();
  if (local != primary && !(legacy && primary < 0)) return;
  std::lock_guard lock(g_held_mutex);
  if (!on) {
    // The menu still shows each held dvar's pick (bo2zm_hold_<dvar> is left as it is).
    g_stop_spawns.store(false, std::memory_order_relaxed);
    for (Held& h : g_held) {
      if (h.menu && !h.release) {
        g_parked.emplace_back(h.name, h.value);
        h.release = true;
      }
    }
  } else {
    for (const auto& [name, value] : g_parked) {
      Held* entry = nullptr;
      for (Held& h : g_held) {
        if (h.name == name) {
          entry = &h;
        }
      }
      if (!entry) {
        entry = &g_held.emplace_back();
        entry->name = name;
      }
      entry->value = value;
      entry->release = false;
      entry->menu = true;
      entry->fixes = 0;
    }
  }
  REXLOG_INFO("[cheats] CHEATS {} ({} held dvars)", on ? "on: the menu's cheats and their kept picks work"
                                                      : "off: every cheat paused, the picks kept",
              g_parked.size());
  if (on) {
    g_parked.clear();
  }
  g_held_dirty.store(true, std::memory_order_relaxed);
  g_held_any.store(true, std::memory_order_relaxed);
}

void bo2zm::cheats::SetMenuCheats(int on) {
  if (!g_primary_bound) g_legacy_boot_picks.store(true, std::memory_order_relaxed);
  const int primary = PrimaryLocalPlayer();
  SetPlayerMaster(primary >= 0 ? primary : 0, on, true);
}

int bo2zm::cheats::LocalPlayerForController(int controller) {
  if (controller < 0 || controller >= kLocalPlayers) return -1;
  std::lock_guard lock(g_actors_mutex);
  for (int local = 0; local < kLocalPlayers; ++local) if (g_controllers[local] == controller) return local;
  return -1;
}
int bo2zm::cheats::PrimaryLocalPlayer() { return g_primary_local.load(std::memory_order_relaxed); }
bool bo2zm::cheats::IsPrimaryController(int controller) {
  const int local = LocalPlayerForController(controller);
  return local >= 0 && local == PrimaryLocalPlayer();
}
bool bo2zm::cheats::GetPlayerOption(int local, std::string_view option, int& value) {
  return local >= 0 && local < kLocalPlayers && StateForLocal(local).options.Get(option, value);
}
bool bo2zm::cheats::SetPlayerOption(int local, std::string_view option, int value) {
  if (local < 0 || local >= kLocalPlayers) return false;
  if (option == "cheats") { SetPlayerMaster(local, value > 0); return true; }
  return StateForLocal(local).options.Set(option, value);
}

int bo2zm::cheats::JumpLevelForState(const uint8_t* base, uint32_t ps) {
  if (!base || !bo2zm::mode::IsZombies()) return 0;
  const Actor actor = ActorForPs(base, ps);
  return actor ? StateForActor(actor).options.Effective(Option::Jump) : 0;
}
float bo2zm::cheats::FieldOfViewScaleForLocal(int local) {
  if (!bo2zm::mode::IsZombies() || local < 0 || local >= kLocalPlayers) return 1.0f;
  std::lock_guard lock(g_actors_mutex);
  const Actor& actor = g_actors[local];
  if (!actor || actor.epoch != g_map_epoch.load(std::memory_order_acquire) || g_map_loading.load(std::memory_order_acquire)) return 1.0f;
  const int level = StateForActor(actor).options.Effective(Option::Fov);
  return float(kFovDegrees[level]) / 65.0f;
}

namespace {
bool PersonalCommand(std::string_view name) {
  return name == "bo2zm_perk" || name == "bo2zm_remove_perks" || name == "bo2zm_max_points" ||
         name == "bo2zm_give_weapon" || name == "bo2zm_pap_weapon";
}
bool QueuePlayer(const Actor& actor, PlayerAction action, std::string_view argument = {}, int pack = -1) {
  if (!actor || g_map_loading.load(std::memory_order_acquire)) return true;
  auto& state = StateForActor(actor);
  const bool pap = pack >= 0 ? pack != 0 : state.options.Effective(Option::GivePap) != 0;
  std::lock_guard lock(g_script_mutex);
  if (actor.epoch != g_map_epoch.load(std::memory_order_acquire)) return true;
  if (action == PlayerAction::RemovePerks) {
    std::erase_if(g_player_requests, [&](const auto& request) {
      return request.actor.controller == actor.controller && request.actor.remote == actor.remote &&
             request.action == PlayerAction::Perk;
    });
  }
  if (action == PlayerAction::Perk || action == PlayerAction::Points || action == PlayerAction::Pap) {
    for (const auto& request : g_player_requests) {
      if (SameActor(request.actor, actor) && request.action == action && request.argument == argument) return true;
    }
  }
  if (g_player_requests.size() >= 64) return true;
  g_player_requests.push_back({actor, action, std::string(argument), pap});
  g_player_waiting.store(true, std::memory_order_release);
  return true;
}
}

static bool MenuHold(bool hold, std::string_view argument);

namespace {
// The world settings a joined match takes from its host (the GAME SPEED, ZOMBIES FREEZE, GRAVITY and UNLIMITED
// SPRINT rows): engine dvars the whole match shares. Each with the menu's values, and the game's own first.
struct WorldDvar {
  std::string_view name;
  std::initializer_list<std::string_view> values;
};
const WorldDvar kWorldDvars[] = {
    {"timescale", {"1", "0.25", "0.5", "1.5", "2"}},
    {"g_ai", {"1", "0"}},
    {"bg_gravity", {"800", "150", "400", "1200"}},
    {"player_sprintUnlimited", {"0", "1"}},
};
const WorldDvar* FindWorldDvar(std::string_view name) {
  for (const auto& dvar : kWorldDvars) if (dvar.name == name) return &dvar;
  return nullptr;
}
bool WorldValue(const WorldDvar& dvar, std::string_view value) {
  for (const auto& allowed : dvar.values) if (allowed == value) return true;
  return false;
}
// The world commands a console that joined another's match sends to its host instead of running them.
bool ForwardedWorldCommand(std::string_view name, std::string_view argument) {
  if (name == "bo2zm_hold" || name == "bo2zm_release") {
    const size_t space = argument.find(' ');
    return space != std::string_view::npos && FindWorldDvar(argument.substr(0, space));
  }
  return name == "bo2zm_powerup" || name == "bo2zm_kill_zombies" || name == "bo2zm_doors" || name == "bo2zm_power" ||
         name == "bo2zm_debris";
}
}  // namespace

bool bo2zm::cheats::RunPlayerCommand(int local, std::string_view name, std::string_view argument) {
  if (local < 0 || local >= kLocalPlayers) return false;
  if (name == "bo2zm_reset_cheats") {
    if (local == PrimaryLocalPlayer()) return RunCommand(name, argument);
    StateForLocal(local).options.ResetGameplay();
    const Actor actor = LocalActor(local);
    std::lock_guard lock(g_script_mutex);
    std::erase_if(g_player_requests, [&](const auto& request) {
      return request.actor.local == local || (actor && request.actor.controller == actor.controller);
    });
    return true;
  }
  if (!PersonalCommand(name)) {
    return local == PrimaryLocalPlayer() ? RunCommand(name, argument) : false;
  }
  if (!bo2zm::mode::IsZombies() || !StateForLocal(local).options.Effective(Option::Cheats)) return true;
  PlayerAction action = PlayerAction::Points;
  if (name == "bo2zm_perk") {
    if (argument.empty() || argument.size() > 40 ||
        argument.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string_view::npos) return true;
    action = PlayerAction::Perk;
  } else if (name == "bo2zm_give_weapon") {
    if (argument.empty() || argument.size() > 48 ||
        argument.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string_view::npos) return true;
    action = PlayerAction::Gun;
  } else if (name == "bo2zm_remove_perks") action = PlayerAction::RemovePerks;
  else if (name == "bo2zm_pap_weapon") action = PlayerAction::Pap;
  if (g_joined.load(std::memory_order_acquire)) {
    // The host runs it on this player's server entity (bo2zm_cheat_sync.cpp).
    const int controller = LocalActor(local).controller;
    bo2zm::cheat_sync::Forward(controller >= 0 ? controller : g_primary_controller.load(std::memory_order_relaxed),
                               name, argument,
                               action == PlayerAction::Gun && StateForLocal(local).options.Effective(Option::GivePap));
    return true;
  }
  return QueuePlayer(LocalActor(local), action, argument);
}

bool bo2zm::cheats::RunCommand(std::string_view name, std::string_view argument) {
  if (PersonalCommand(name)) return RunPlayerCommand(PrimaryLocalPlayer(), name, argument);
  if (!bo2zm::mode::IsZombies()) {
    return MenuCheatCommand(name) || name == "bo2zm_release" || name == "bo2zm_reset_cheats";
  }
  if (MenuCheatCommand(name) && !MenuCheats()) return true;
  if (name == "bo2zm_reset_cheats") {
    CheatsToNormal();
    const int primary = PrimaryLocalPlayer();
    {
      std::lock_guard lock(g_script_mutex);
      std::erase_if(g_player_requests, [&](const auto& request) { return request.actor.local == primary; });
    }
    std::lock_guard lock(g_held_mutex);
    for (Held& h : g_held) h.release |= !h.mirror;  // a joined match's world settings are its host's
    g_parked.clear();
    g_held_dirty.store(true, std::memory_order_relaxed);
    g_held_any.store(true, std::memory_order_relaxed);
    return true;
  }
  if (g_joined.load(std::memory_order_acquire) && ForwardedWorldCommand(name, argument)) {
    const int controller = g_primary_controller.load(std::memory_order_relaxed);
    if (name == "bo2zm_hold" || name == "bo2zm_release") {
      const size_t space = argument.find(' ');
      std::lock_guard lock(g_held_mutex);
      Show(std::string(argument.substr(0, space)), std::string(argument.substr(space + 1)));  // the menu's pick
      g_held_dirty.store(true, std::memory_order_relaxed);
    }
    bo2zm::cheat_sync::Forward(controller, name, argument, false);
    return true;
  }
  if ((name == "bo2zm_powerup" || name == "bo2zm_kill_zombies" || name == "bo2zm_doors" ||
       name == "bo2zm_power" || name == "bo2zm_debris") &&
      (g_map_loading.load(std::memory_order_acquire) || !PrimaryActor())) return true;
  if (name == "bo2zm_powerup") {
    if (argument.empty() || argument.size() > 32 ||
        argument.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string_view::npos) return true;
    return QueuePlayer(PrimaryActor(), PlayerAction::Powerup, argument);
  }
  if (name == "bo2zm_kill_zombies") { KillZombiesNow(); return true; }
  if (name == "bo2zm_doors" || name == "bo2zm_power" || name == "bo2zm_debris") {
    if ((name == "bo2zm_doors" && argument != "open" && argument != "close") ||
        (name == "bo2zm_power" && argument != "on" && argument != "off") ||
        (name == "bo2zm_debris" && argument != "remove" && argument != "restore")) return true;
    const Actor target = PrimaryActor();
    std::lock_guard lock(g_script_mutex);
    if (target.epoch != g_map_epoch.load(std::memory_order_acquire) || g_map_loading.load(std::memory_order_acquire)) return true;
    g_world_target = target;
    if (name == "bo2zm_doors") g_doors_request = argument == "open" ? 1 : 2;
    else if (name == "bo2zm_power") g_power_request = argument == "on" ? 1 : 2;
    else g_debris_request = argument == "remove" ? 1 : 2;
    g_world_waiting.store(true, std::memory_order_relaxed);
    return true;
  }
  const bool hold = name == "bo2zm_hold";
  if (!hold && name != "bo2zm_release") {
    return false;
  }
  return MenuHold(hold, argument);
}

// bo2zm_hold <dvar> <value>; bo2zm_release <dvar> <the menu's value for the game's own choice>.
static bool MenuHold(bool hold, std::string_view argument) {
  const std::string_view name = hold ? "bo2zm_hold" : "bo2zm_release";
  const size_t space = argument.find(' ');
  const std::string dvar(argument.substr(0, space));
  const std::string value(space == std::string_view::npos ? std::string_view() : argument.substr(space + 1));
  if (dvar.empty() || value.empty()) {
    REXLOG_WARN("[cheats] {}: needs a dvar and a value", name);
    return true;
  }
  std::lock_guard lock(g_held_mutex);
  Show(dvar, value);
  g_held_dirty.store(true, std::memory_order_relaxed);
  g_held_any.store(true, std::memory_order_relaxed);
  if (!hold) {
    std::erase_if(g_parked, [&](const auto& parked) { return parked.first == dvar; });
  }
  Held* entry = nullptr;
  for (Held& h : g_held) {
    if (h.name == dvar) {
      entry = &h;
    }
  }
  if (!entry) {
    if (!hold) {
      return true;  // not held: already the game's
    }
    entry = &g_held.emplace_back();
    entry->name = dvar;
  }
  entry->value = value;
  entry->release = !hold;
  entry->menu = true;
  entry->fixes = 0;
  return true;
}

static void PrepareServerState(const Actor& actor, PlayerState& state) {
  if (state.server_epoch != actor.epoch || state.server_generation != actor.generation) {
    state.server_epoch = actor.epoch;
    state.server_generation = actor.generation;
    state.perks_active = state.perks_was_down = false;
    state.noted_perks[0] = state.noted_perks[1] = 0;
    state.perks_gone_since = state.perks_quiet_until = {};
  }

}

static void RunPlayerRequests(PPCContext& ctx, uint8_t* base) {
  if (!g_player_waiting.load(std::memory_order_acquire)) return;
  std::vector<PlayerRequest> requests;
  {
    std::lock_guard lock(g_script_mutex);
    requests.swap(g_player_requests);
    g_player_waiting.store(false, std::memory_order_release);
  }
  // No host lock or implicit actor selection survives into guest code.
  for (const auto& request : requests) {
    const Actor actor = request.actor;
    if (!actor.client || !CurrentActor(base, actor) || !StateForActor(actor).options.Effective(Option::Cheats)) continue;
    PrepareServerState(actor, StateForActor(actor));
    if (request.action == PlayerAction::Points) {
      Store32(base, actor.client + kClientScore, kMaxPoints);
      continue;
    }
    if (request.action == PlayerAction::Powerup) {
      if (actor.remote < 0 && actor.local != bo2zm::cheats::PrimaryLocalPlayer()) continue;
      if (FreeEntities(base) < kSpareEntities) continue;
      constexpr float radians = 3.14159265f / 180.0f;
      const float yaw = LoadFloat(base, actor.client + kPsViewAngles + 4) * radians;
      const Vec3 feet = LoadVec(base, actor.entity + kEntOrigin);
      const Vec3 spot{feet.x + std::cos(yaw) * kPowerupDistance, feet.y + std::sin(yaw) * kPowerupDistance, feet.z};
      RunScript(ctx, base, kPowerupScript, "specific_powerup_drop",
                {{ScriptArg::kString, request.argument}, {ScriptArg::kVector, {}, spot}});
      continue;
    }
    if (!IsUp(base, actor.client) || Load32(base, actor.client + kClientSessionState) != 0) continue;
    switch (request.action) {
      case PlayerAction::Perk: GivePerk(ctx, base, actor, request.argument); break;
      case PlayerAction::RemovePerks: RemovePerks(ctx, base, actor); break;
      case PlayerAction::Gun: GiveGun(ctx, base, actor, request.argument, request.pap); break;
      case PlayerAction::Pap: PackGunInHand(ctx, base, actor); break;
      default: break;
    }
  }
}

int bo2zm::cheats::GetCrosshairOverlays(CrosshairOverlay* out, int capacity) {
  if (!out || capacity <= 0) return 0;
  std::lock_guard lock(g_crosshair_mutex);
  const int count = std::min(g_crosshair_count, capacity);
  std::copy_n(g_crosshair_overlays.begin(), count, out);
  return count;
}

void bo2zm::cheats::BeginMapLoad() {
  g_map_loading.store(true, std::memory_order_release);
  g_map_epoch.fetch_add(1, std::memory_order_acq_rel);
  {
    std::lock_guard lock(g_actors_mutex);
    g_actors = {};
    g_remote_actors = {};
  }
  g_remote_count.store(0, std::memory_order_relaxed);
  g_forget_remote.store(true, std::memory_order_release);
  for (auto& state : g_players) {
    state.in_match.store(false, std::memory_order_relaxed);
    state.aiming.store(false, std::memory_order_relaxed);
    state.options.CancelCombo();
  }
  g_active_locals.store(0, std::memory_order_relaxed);
  g_pending_round = 0;
  g_in_match.store(false, std::memory_order_relaxed);
  g_kill_zombies.store(false, std::memory_order_relaxed);
  ReleaseHold();
  {
    std::lock_guard lock(g_script_mutex);
    g_player_requests.clear();
    g_player_waiting.store(false, std::memory_order_release);
    g_round_request = 0;
    g_doors_request = g_power_request = g_debris_request = 0;
    g_script_waiting.store(false, std::memory_order_relaxed);
    g_world_waiting.store(false, std::memory_order_relaxed);
  }
  {
    std::lock_guard lock(g_crosshair_mutex);
    g_crosshair_overlays = {};
    g_crosshair_count = 0;
  }
  {
    std::lock_guard lock(g_blasts_mutex);
    g_blasts.clear();
    g_blasts_waiting.store(false, std::memory_order_relaxed);
  }
  // A solo raised jump must not leak its shared constant into a newly loaded
  // split-screen server before the first main-frame actor publication.
  if (auto* runtime = rex::Runtime::instance(); runtime && runtime->memory()) {
    RestoreStockJump(runtime->memory()->virtual_membase());
  }
}

void bo2zm::cheats::EndMapLoad() { g_map_loading.store(false, std::memory_order_release); }

// The cheats that run game scripts (see the top of the file): Server thread, right after each server frame.
void bo2zm::cheats::ServerFrame(PPCContext& ctx, uint8_t* base) {
  if (!bo2zm::mode::IsZombies() || g_map_loading.load(std::memory_order_acquire)) return;
  KillZombies(ctx, base);
  if (Client(base)) ReadPerkFlags(ctx, base);
  SettleDebris(base);
  RunWorldRequests(ctx, base);
  RunScriptRequests(ctx, base);
  RunPlayerRequests(ctx, base);
  KeepRoundRequest(ctx, base);
  for (int local = 0; local < kLocalPlayers; ++local) {
    const Actor actor = LocalActor(local);
    if (!CurrentActor(base, actor)) continue;
    auto& state = StateForActor(actor);
    PrepareServerState(actor, state);
    KeepPerks(ctx, base, actor, state);
  }
  for (int remote = 0; g_remote_count.load(std::memory_order_relaxed) && remote < kRemotePlayers; ++remote) {
    const Actor actor = RemoteActor(remote);
    if (!CurrentActor(base, actor)) continue;
    auto& state = StateForActor(actor);
    PrepareServerState(actor, state);
    KeepPerks(ctx, base, actor, state);
  }
  ReportParents(base);
}

int bo2zm::cheats::ScriptParentsInUse() {
  return g_parents_in_use.load(std::memory_order_relaxed);
}

static void PrepareMainState(const Actor& actor, PlayerState& state) {
  if (state.main_epoch != actor.epoch || state.main_generation != actor.generation) {
    state.main_epoch = actor.epoch;
    state.main_generation = actor.generation;
    state.sized_weapon = state.left_weapon = 0;
    state.clip_size = state.left_size = 0;
    state.god_was_on = state.third_was_on = state.ignore_was_on = false;
    state.last_health = 0;
  }
}

void bo2zm::cheats::EveryFrame(PPCContext& ctx, uint8_t* base) {
  static bool dof_held = false;
  UpdateActors(base);
  const Actor primary = PrimaryActor();
  g_in_match.store(CurrentActor(base, primary), std::memory_order_relaxed);
  TakeSpot(base);
  // A map load ends when the server's scripts start (EndMapLoad); a console that joined another's System Link game runs
  // no server, so there the client being in the match ends it.
  if (g_map_loading.load(std::memory_order_acquire) && !primary && bo2zm::preset::ClientInGame() &&
      !LocalServerRunning(base)) EndMapLoad();
  bo2zm::cheat_sync::TraceFrame(g_map_loading.load(std::memory_order_acquire), bool(primary));
  if (!bo2zm::mode::IsZombies()) {
    PublishCrosshairOverlays(base);
    bo2zm::cheat_sync::EveryFrame(ctx, base);  // Multiplayer: the mod menus' tables only
    SlowWhileBuilding(base);
    KeepHeldDvars(ctx, base);
    return;
  }
  if (g_map_loading.load(std::memory_order_acquire)) return;
  for (int local = 0; local < kLocalPlayers; ++local) {
    const Actor actor = LocalActor(local);
    if (!CurrentActor(base, actor)) continue;
    auto& state = StateForActor(actor);
    PrepareMainState(actor, state);
    KeepBit(base, actor.client, kClientFlags, kInvulnerable, state.options.Effective(Option::God), true,
            state.god_was_on);
    ReportGod(base, actor, state);
    KeepAmmo(ctx, base, actor, state);
    KeepIgnored(base, actor, state);
    KeepByte(base, actor.client, kClientThirdPerson, state.options.Effective(Option::Third), state.third_was_on);
    TakeComboPresses(state, actor.generation);
  }
  UpdateRemoteActors(base);
  for (int remote = 0; g_remote_count.load(std::memory_order_relaxed) && remote < kRemotePlayers; ++remote) {
    const Actor actor = RemoteActor(remote);
    if (!CurrentActor(base, actor)) continue;
    auto& state = StateForActor(actor);
    PrepareMainState(actor, state);
    KeepBit(base, actor.client, kClientFlags, kInvulnerable, state.options.Effective(Option::God), true,
            state.god_was_on);
    ReportGod(base, actor, state);
    KeepAmmo(ctx, base, actor, state);
    KeepIgnored(base, actor, state);
    KeepByte(base, actor.client, kClientThirdPerson, state.options.Effective(Option::Third), state.third_was_on);
  }
  bo2zm::cheat_sync::EveryFrame(ctx, base);
  UpdateCatalog(ctx, base);
  RunTestCommands(ctx, base);
  if (g_active_locals.load(std::memory_order_relaxed) == 0) DropScriptRequests();
  PlayBlasts(ctx, base);
  auto& state = PrimaryState();
  const bool third = primary && state.options.Effective(Option::Third);
  if (third != dof_held) {
    dof_held = third;
    std::lock_guard lock(g_held_mutex);
    HoldLocked("r_dof_enable", "0", third);
  }
  RestoreStockJump(base);
  SlowWhileBuilding(base);
  KeepHeldDvars(ctx, base);
  WatchSvCheats(base);
  PublishCrosshairOverlays(base);
}

// The server frame (sub_82336790, on the Server thread: it runs the game's script threads through sub_825444F8), and
// right after it the cheats that run game scripts (see the top of the file).
REX_EXTERN(__imp__sub_82336790);
REX_HOOK_RAW(sub_82336790) {
  __imp__sub_82336790(ctx, base);
  bo2zm::cheats::ServerFrame(ctx, base);
}

REX_EXTERN(__imp__sub_8269AE30);

// The game's normal jump speed for player state ps (velocity z at the start of a jump of the game's height).
static float NormalJumpSpeed(uint8_t* base, uint32_t ps) {
  return std::sqrt(2.0f * float(std::max(int32_t(Load32(base, ps + kPsGravity)), 0)) * kJumpLevels[0]);
}

// Smooth raised jumps (see the top of the file): the rise speed for the height climbed so far; falls capped.
static void SmoothJump(uint8_t* base, uint32_t ps, float height, Vec3& v) {
  if (v.z > 0.0f && (Load32(base, ps + kPsPmFlags) & kPmfJumping)) {
    const float gravity = float(std::max(int32_t(Load32(base, ps + kPsGravity)), 0));
    const float climbed = std::max(LoadFloat(base, ps + kPsOrigin + 8) - LoadFloat(base, ps + kPsJumpOriginZ), 0.0f);
    const float start = NormalJumpSpeed(base, ps);
    const float boost = std::sqrt(start * start + 2.0f * kJumpBoost * climbed);
    const float brake = std::sqrt(std::max(2.0f * gravity * (height - climbed), 0.0f));
    v.z = std::min({kJumpCruise, boost, brake});
  }
  v.z = std::max(v.z, -kJumpCruise);
}

// In the air: the game's normal step, then smooth raised jumps, and feather fall (a slow fall and steering).
static void AirStep(PPCContext& ctx, uint8_t* base, uint32_t pm, uint32_t ps, int jump, bool feather) {
  const int32_t ms = int32_t(Load32(base, pm + kPmCmdTime)) - int32_t(Load32(base, ps + kPsCommandTime));
  const float dt = std::clamp(float(ms) / 1000.0f, 0.0f, 0.25f);
  const float stick_forward = float(int8_t(base[pm + kPmForward])) / 127.0f;
  const float stick_right = float(int8_t(base[pm + kPmRight])) / 127.0f;
  __imp__sub_8269AE30(ctx, base);
  Vec3 v = LoadVec(base, ps + kPsVelocity);
  if (jump != 0) {
    SmoothJump(base, ps, kJumpLevels[jump], v);
  }
  if (!feather || v.z >= -1.0f) {
    if (jump != 0) {
      StoreVec(base, ps + kPsVelocity, v);
    }
    return;  // no feather fall, or on the ground or still rising
  }
  v.z = std::max(v.z, -kFeatherFall);
  constexpr float kRadians = 3.14159265f / 180.0f;
  const float yaw = LoadFloat(base, ps + kPsViewAngles + 4) * kRadians;
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  float wx = cy * stick_forward + sy * stick_right;
  float wy = sy * stick_forward - cy * stick_right;
  const float wish = std::sqrt(wx * wx + wy * wy);
  if (wish > 1.0f) {
    wx /= wish;
    wy /= wish;
  }
  v.x += wx * kGlideAccel * dt;
  v.y += wy * kGlideAccel * dt;
  const float speed = std::sqrt(v.x * v.x + v.y * v.y);
  if (speed > kGlideSpeed) {
    v.x *= kGlideSpeed / speed;
    v.y *= kGlideSpeed / speed;
  }
  StoreVec(base, ps + kPsVelocity, v);
}


// The angle a fraction of the way from a to b (degrees, the short way round).
static float LerpAngle(float a, float b, float t) {
  float delta = std::fmod(b - a, 360.0f);
  if (delta > 180.0f) {
    delta -= 360.0f;
  } else if (delta < -180.0f) {
    delta += 360.0f;
  }
  return a + delta * t;
}

// The camera trace (see the top of the file): from start towards end, stopping at what mask hits; the point reached.
static Vec3 TraceCamera(PPCContext& ctx, uint8_t* base, uint32_t cg, const Vec3& start, const Vec3& end,
                        uint32_t mask) {
  const PPCContext saved = ctx;
  const uint32_t sp = (ctx.r1.u32 - 1024) & ~0xFu;
  StoreVec(base, sp + 256, start);
  StoreVec(base, sp + 268, end);
  StoreVec(base, sp + 280, end);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = cg;
  ctx.r4.u64 = sp + 256;
  ctx.r5.u64 = sp + 268;
  ctx.r6.u64 = mask;
  ctx.r7.u64 = sp + 280;
  ctx.r8.u64 = 1;
  ctx.r9.u64 = 1;
  bo2zm::guest::Call<kCameraTrace>(ctx, base);
  const Vec3 reached = LoadVec(base, sp + 280);
  ctx = saved;
  return reached;
}


// The client's plain trace (sub_8225CBF8: result, start, mins, maxs, end, entity to pass, mask, parameters; the
// fraction reached at result + 16), as a line from start to end: the point it reaches. It passes the local player
// the way the camera trace does (cg + 0x481F4, else cg + 0x48248).
static Vec3 TraceShot(PPCContext& ctx, uint8_t* base, uint32_t cg, const Vec3& start, const Vec3& end) {
  const PPCContext saved = ctx;
  const uint32_t sp = (ctx.r1.u32 - 1024) & ~0xFu;
  const uint32_t params = sp + 80;   // 40 bytes, as sub_823F70C0 sets them up
  const uint32_t result = sp + 144;  // 128 bytes
  const uint32_t from = sp + 320;
  const uint32_t to = sp + 332;
  const uint32_t zero = sp + 344;
  for (uint32_t i = 0; i < 40; i += 4) {
    Store32(base, params + i, (i == 16 || i == 20) ? kEntityNone : 0);
  }
  std::memset(base + result, 0, 128);
  StoreVec(base, from, start);
  StoreVec(base, to, end);
  StoreVec(base, zero, {0.0f, 0.0f, 0.0f});
  uint32_t pass = Load32(base, cg + kCgPassEntity);
  if (pass == kEntityNone) {
    pass = Load32(base, cg + kCgPassEntityOther);
  }
  ctx.r1.u64 = sp;
  ctx.r3.u64 = result;
  ctx.r4.u64 = from;
  ctx.r5.u64 = zero;
  ctx.r6.u64 = zero;
  ctx.r7.u64 = to;
  ctx.r8.u64 = pass;
  ctx.r9.u64 = kShotMask;
  ctx.r10.u64 = params;
  bo2zm::guest::Call<kLineTrace>(ctx, base);
  const float fraction = std::clamp(LoadFloat(base, result + 16), 0.0f, 1.0f);
  ctx = saved;
  return {start.x + (end.x - start.x) * fraction, start.y + (end.y - start.y) * fraction,
          start.z + (end.z - start.z) * fraction};
}

// The third person view: over the shoulder while aiming (see the top of the file).
REX_EXTERN(__imp__sub_8223A410);
REX_HOOK_RAW(sub_8223A410) {
  if (!bo2zm::mode::IsZombies()) {
    __imp__sub_8223A410(ctx, base);
    return;
  }
  const int local = int(ctx.r3.u32);
  const Actor actor = LocalActor(local);
  if (!CurrentActor(base, actor)) { __imp__sub_8223A410(ctx, base); return; }
  auto& state = StateForActor(actor);
  if (state.camera_epoch != actor.epoch || state.camera_generation != actor.generation) {
    state.camera_epoch = actor.epoch;
    state.camera_generation = actor.generation;
    state.shoulder_blend = 0.0f;
    state.shoulder_last = {};
  }
  using Clock = std::chrono::steady_clock;
  auto& blend = state.shoulder_blend;
  auto& last = state.shoulder_last;
  const bool aim = state.options.Effective(Option::Third) && state.aiming.load(std::memory_order_relaxed);
  if (!aim && blend <= 0.0f) [[likely]] {
    last = {};
    __imp__sub_8223A410(ctx, base);
    return;
  }
  const auto now = Clock::now();
  const float dt = last == Clock::time_point{} ? 0.0f : std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
  last = now;
  // Only the first two aims are logged: each log line costs the main thread some time.
  auto& aim_logged = state.aim_logged;
  auto& shift_logged = state.shift_logged;
  auto& aims_logged = state.aims_logged;
  if (aim != aim_logged) {
    aim_logged = aim;
    if (aims_logged < 4) {
      ++aims_logged;
      shift_logged = false;
      REXLOG_INFO("[cheats] third person aiming {}", aim ? "started" : "stopped");
    }
  }
  blend = aim ? std::min(1.0f, blend + kShoulderEase * dt) : std::max(0.0f, blend - kShoulderEase * dt);
  const uint32_t range_dvar = Load32(base, kThirdPersonRangeDvar);
  if (blend <= 0.0f || !range_dvar) [[likely]] {
    __imp__sub_8223A410(ctx, base);
    return;
  }
  const uint32_t cg = actor.cg;
  const uint32_t origin = cg + kCgViewOrigin;
  const uint32_t camera_axis = cg + kCgViewAxis;
  const Vec3 entry = LoadVec(base, origin);
  const float range = LoadFloat(base, range_dvar + kDvarCurrent);
  const float used_range = range + (kShoulderRange - range) * blend;
  __imp__sub_8223A410(ctx, base);
  if (!CurrentActor(base, actor)) return;
  const Vec3 camera = LoadVec(base, origin);
  if (camera.x == entry.x && camera.y == entry.y && camera.z == entry.z) {
    return;  // no third person camera this frame
  }

  constexpr float kRadians = 3.14159265f / 180.0f;
  // The game's construction (see the top of the file): from the view angles it starts from, the camera backs off from
  // kCameraLift above the head along half the view pitch (level in cg_thirdPersonMode 2), and looks at the point
  // kFocusDistance ahead of the head along the view.
  const uint32_t mode_dvar = Load32(base, kThirdPersonModeDvar);
  const bool level = mode_dvar && Load32(base, mode_dvar + kDvarCurrent) == 2;
  const float view_pitch = level ? 0.0f : LoadFloat(base, cg + kCgBaseAngles) * kRadians;
  const float yaw = LoadFloat(base, cg + kCgBaseAngles + 4) * kRadians;
  const auto forward = [](float pitch, float heading) -> Vec3 {
    return {std::cos(pitch) * std::cos(heading), std::cos(pitch) * std::sin(heading), -std::sin(pitch)};
  };
  const auto along = [](const Vec3& from, const Vec3& direction, float distance) -> Vec3 {
    return {from.x + direction.x * distance, from.y + direction.y * distance, from.z + direction.z * distance};
  };
  const Vec3 orbit = forward(view_pitch * kOrbitPitchScale, yaw);
  const Vec3 view = forward(view_pitch, yaw);
  const Vec3 right = {std::sin(yaw), -std::cos(yaw), 0.0f};
  const Vec3 lifted = along(camera, orbit, range);
  const Vec3 head = {lifted.x, lifted.y, lifted.z - kCameraLift};
  const Vec3 focus = along(head, view, kFocusDistance);

  // What the player aims at: the bullets' trace from the head along the view; the game's focus point when the trace
  // ends right away (it started inside something).
  Vec3 target = TraceShot(ctx, base, cg, head, along(head, view, kAimReach));
  const auto distance = [](const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
  };
  if (distance(target, head) < kMinAimDistance) {
    target = focus;
  }
  // Over the shoulder: the camera moves right (and a little up), stopping at walls.
  const Vec3 moved = TraceCamera(ctx, base, cg, camera,
                                 {camera.x + orbit.x * (range - used_range) + right.x * kShoulderRight * blend,
                                  camera.y + orbit.y * (range - used_range) + right.y * kShoulderRight * blend,
                                  camera.z + orbit.z * (range - used_range) + kShoulderUp * blend},
                                 kCameraMask);
  StoreVec(base, origin, moved);

  // And turns to the aim point, never far from where the game's camera looks (a close aim point, such as a wall a
  // step away, would swing it round): beyond kMaxTurn it turns to the game's focus point instead. The game's camera
  // angles: its yaw is the view's less cg_thirdPersonAngle, its pitch looks at the focus point from where the game put
  // the camera; the angles become the view axis through the game's own function (sub_8248E540: angles, axis out),
  // so the axis is in its layout.
  const uint32_t angle_dvar = Load32(base, kThirdPersonAngleDvar);
  const Vec3 to_focus = {focus.x - camera.x, focus.y - camera.y, focus.z - camera.z};
  const float game_pitch =
      -std::atan2(to_focus.z, std::max(std::sqrt(to_focus.x * to_focus.x + to_focus.y * to_focus.y), 1.0f)) / kRadians;
  const float game_yaw = yaw / kRadians - (angle_dvar ? LoadFloat(base, angle_dvar + kDvarCurrent) : 0.0f);
  const float game_roll = LoadFloat(base, cg + kCgBaseAngles + 8);
  const auto look = [&](const Vec3& at, float& pitch, float& heading) {
    const Vec3 d = {at.x - moved.x, at.y - moved.y, at.z - moved.z};
    const float flat = std::max(std::sqrt(d.x * d.x + d.y * d.y), 1.0f);
    pitch = -std::atan2(d.z, flat) / kRadians;
    heading = std::atan2(d.y, d.x) / kRadians;
  };
  float aim_pitch = 0.0f;
  float aim_yaw = 0.0f;
  look(target, aim_pitch, aim_yaw);
  bool fallback = false;
  if (std::abs(LerpAngle(game_pitch, aim_pitch, 1.0f) - game_pitch) > kMaxTurn ||
      std::abs(LerpAngle(game_yaw, aim_yaw, 1.0f) - game_yaw) > kMaxTurn) {
    look(focus, aim_pitch, aim_yaw);
    fallback = true;
  }
  {
    const PPCContext saved = ctx;
    const uint32_t sp = (ctx.r1.u32 - 1024) & ~0xFu;
    StoreVec(base, sp + 256, {LerpAngle(game_pitch, aim_pitch, blend), LerpAngle(game_yaw, aim_yaw, blend), game_roll});
    ctx.r1.u64 = sp;
    ctx.r3.u64 = sp + 256;
    ctx.r4.u64 = camera_axis;
    bo2zm::guest::Call<kAnglesToAxis>(ctx, base);
    ctx = saved;
  }
  if (blend >= 1.0f && !shift_logged) {
    shift_logged = true;
    REXLOG_INFO("[cheats] over the shoulder: head ({:.0f} {:.0f} {:.0f}), camera ({:.0f} {:.0f} {:.0f}) -> ({:.0f} "
                "{:.0f} {:.0f}), aim point ({:.0f} {:.0f} {:.0f}){}, view pitch {:.1f} yaw {:.1f}, camera pitch {:.1f} "
                "-> {:.1f}, yaw {:.1f} -> {:.1f}",
                head.x, head.y, head.z, camera.x, camera.y, camera.z, moved.x, moved.y, moved.z, target.x, target.y,
                target.z, fallback ? " (too close: focus point)" : "", view_pitch / kRadians, yaw / kRadians,
                game_pitch, aim_pitch, game_yaw, aim_yaw);
  }
}

// The game's jump start: at a raised level, the selected player's jump starts at the game's normal jump speed (see the top of
// the file); the movement step speeds it up from there.
REX_EXTERN(__imp__sub_82687E20);
REX_HOOK_RAW(sub_82687E20) {
  const uint32_t pm = ctx.r3.u32;
  const Actor actor = bo2zm::mode::IsZombies() && AnyOption(Option::Jump) ? ActorForPm(base, pm) : Actor{};
  __imp__sub_82687E20(ctx, base);
  if (!CurrentActor(base, actor) || StateForActor(actor).options.Effective(Option::Jump) == 0) [[likely]] {
    return;
  }
  const uint32_t ps = Load32(base, pm + kPmPs);
  if (ps < 0x10000 || !SameActor(ActorForPs(base, ps), actor)) {
    return;
  }
  const float start = NormalJumpSpeed(base, ps);
  if (LoadFloat(base, ps + kPsVelocity + 8) > start) {
    StoreFloat(base, ps + kPsVelocity + 8, start);
  }
}

// A bullet hit: explosive bullets for the selected player (see the top of the file).
REX_EXTERN(__imp__sub_82300AF0);
REX_HOOK_RAW(sub_82300AF0) {
  if (!AnyOption(Option::Explosive)) { __imp__sub_82300AF0(ctx, base); return; }
  const uint32_t attacker = ctx.r7.u32;
  const Actor actor = bo2zm::mode::IsZombies() ? ActorForEntity(base, attacker) : Actor{};
  if (!actor || !StateForActor(actor).options.Effective(Option::Explosive)) [[likely]] {
    __imp__sub_82300AF0(ctx, base);
    return;
  }
  const uint32_t trace = ctx.r5.u32;
  const uint32_t weapon = ctx.r6.u32;
  __imp__sub_82300AF0(ctx, base);
  if (!CurrentActor(base, actor) || trace < 0x10000) {
    return;
  }
  const Vec3 hit = LoadVec(base, trace + kTraceHit);
  RadiusDamage(ctx, base, hit, attacker, kBlastInner, kBlastOuter, kBlastRadius, weapon);
  static int logged = 0;
  if (logged < 5) {
    ++logged;
    REXLOG_INFO("[cheats] explosive bullet hit at ({:.0f} {:.0f} {:.0f}), weapon {}", hit.x, hit.y, hit.z, weapon);
  }
  if (actor.remote >= 0) {
    // The effect is the client's: the console the player plays on gets the hit point (bo2zm_cheat_sync.cpp).
    std::lock_guard lock(g_blasts_mutex);
    if (g_remote_blasts.size() < 16) g_remote_blasts.push_back({actor.xuid, hit});
    return;
  }
  std::lock_guard lock(g_blasts_mutex);
  g_blasts_waiting.store(true, std::memory_order_relaxed);
  if (g_blasts.size() < 16) {
    g_blasts.push_back({actor, hit});
  }
}

// The script method spawnactor: nothing while spawns are stopped (see the top of the file).
REX_EXTERN(__imp__sub_823696B8);
REX_HOOK_RAW(sub_823696B8) {
  if (bo2zm::mode::IsZombies() && g_stop_spawns.load(std::memory_order_relaxed)) [[unlikely]] {
    return;
  }
  __imp__sub_823696B8(ctx, base);
}

// The script method setclientfieldtoplayer: skipped for a field the map never registered (see the top of the file).
REX_EXTERN(__imp__sub_823743A8);
REX_HOOK_RAW(sub_823743A8) {
  if (!bo2zm::mode::IsZombies()) {
    __imp__sub_823743A8(ctx, base);
    return;
  }
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 256) & ~0xFu;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = 1;  // the field name, the method's first argument
  bo2zm::guest::Call<kScriptGetString>(ctx, base);
  const uint32_t name = ctx.r3.u32;
  ctx.r3.u64 = kPlayerClientFields;
  ctx.r4.u64 = name;
  ctx.r5.u64 = 1;
  bo2zm::guest::Call<kClientFieldFind>(ctx, base);
  const uint32_t field = ctx.r3.u32;
  ctx = saved;
  if (field == 0) [[unlikely]] {
    static int logged = 0;
    if (logged < 5) {
      ++logged;
      REXLOG_INFO("[cheats] setclientfieldtoplayer: the map has no client field {}; skipped",
                  name >= 0x40000000u ? reinterpret_cast<const char*>(base + name) : "?");
    }
    return;
  }
  __imp__sub_823743A8(ctx, base);
}

// The weapon kick of a shot: none for the selected player with no recoil (see the top of the file).
REX_EXTERN(__imp__sub_826BAF08);
REX_HOOK_RAW(sub_826BAF08) {
  if (!AnyOption(Option::NoRecoil)) { __imp__sub_826BAF08(ctx, base); return; }
  const uint32_t ps = ctx.r3.u32;
  const Actor actor = bo2zm::mode::IsZombies() ? ActorForPs(base, ps) : Actor{};
  if (!actor || !StateForActor(actor).options.Effective(Option::NoRecoil))
      [[likely]] {
    __imp__sub_826BAF08(ctx, base);
    return;
  }
  const uint32_t aim = ctx.r4.u32;
  const uint32_t view = ctx.r5.u32;
  if (!GuestRange(aim, 8) || !GuestRange(view, 12)) [[unlikely]] {
    __imp__sub_826BAF08(ctx, base);
    return;
  }
  uint8_t aim_before[8];
  uint8_t view_before[12];
  std::memcpy(aim_before, base + aim, sizeof(aim_before));
  std::memcpy(view_before, base + view, sizeof(view_before));
  __imp__sub_826BAF08(ctx, base);
  std::memcpy(base + aim, aim_before, sizeof(aim_before));
  std::memcpy(base + view, view_before, sizeof(view_before));
}

// The game's damage function: nothing hurts the selected player in god mode.
REX_EXTERN(__imp__sub_82325CA8);
REX_HOOK_RAW(sub_82325CA8) {
  const bool test_all_god = REXCVAR_GET(bo2zm_test_all_god);
  if (!test_all_god && !AnyOption(Option::God)) { __imp__sub_82325CA8(ctx, base); return; }
  const Actor actor = bo2zm::mode::IsZombies() ? ActorForEntity(base, ctx.r3.u32) : Actor{};
  if (actor && (test_all_god || StateForActor(actor).options.Effective(Option::God))) [[unlikely]] {
    StateForActor(actor).god_blocked.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  __imp__sub_82325CA8(ctx, base);
}

// The movement step: fly modes, smooth raised jumps and feather fall for the selected player.
namespace {
std::atomic<bool> g_hold{false};
std::mutex g_hold_mutex;
Actor g_hold_actor;
Vec3 g_hold_feet{};         // under g_hold_mutex
float g_hold_pitch = 0.0f;  // under g_hold_mutex
float g_hold_yaw = 0.0f;    // under g_hold_mutex

// Test spots (HoldAt): the game's own step, then the selected player back at the spot, still, looking where the spot says. The
// view is the command's angles plus ps->delta_angles, so the delta takes the difference and later steps keep the view
// there whatever the stick does. The server's step and the client's prediction (its copy of ps) both come here.
void HoldStep(PPCContext& ctx, uint8_t* base, const Actor& actor) {
  const uint32_t pm = ctx.r3.u32;
  const uint32_t ps = Load32(base, pm + kPmPs);
  __imp__sub_8269AE30(ctx, base);
  if (!CurrentActor(base, actor) || !SameActor(ActorForPs(base, ps), actor)) {
    return;
  }
  Vec3 feet;
  float pitch, yaw;
  {
    std::lock_guard lock(g_hold_mutex);
    feet = g_hold_feet;
    pitch = g_hold_pitch;
    yaw = g_hold_yaw;
  }
  StoreVec(base, ps + kPsOrigin, feet);
  StoreVec(base, ps + kPsVelocity, {0.0f, 0.0f, 0.0f});
  const Vec3 view = LoadVec(base, ps + kPsViewAngles);
  const Vec3 delta = LoadVec(base, ps + kPsDeltaAngles);
  StoreVec(base, ps + kPsDeltaAngles, {delta.x + pitch - view.x, delta.y + yaw - view.y, delta.z});
  StoreVec(base, ps + kPsViewAngles, {pitch, yaw, view.z});
}
}  // namespace

void bo2zm::cheats::HoldAt(const float feet[3], float pitch, float yaw) {
  if (!bo2zm::mode::IsZombies()) {
    return;
  }
  const Actor actor = PrimaryActor();
  if (!actor) return;
  {
    std::lock_guard lock(g_hold_mutex);
    g_hold_actor = actor;
    g_hold_feet = {feet[0], feet[1], feet[2]};
    g_hold_pitch = pitch;
    g_hold_yaw = yaw;
  }
  g_hold.store(true, std::memory_order_relaxed);
}

void bo2zm::cheats::ReleaseHold() {
  g_hold.store(false, std::memory_order_relaxed);
}

void bo2zm::cheats::KillZombiesNow() {
  if (!bo2zm::mode::IsZombies()) {
    return;
  }
  const Actor actor = PrimaryActor();
  if (!actor || g_map_loading.load(std::memory_order_acquire)) return;
  std::lock_guard lock(g_script_mutex);
  if (actor.epoch != g_map_epoch.load(std::memory_order_acquire) || g_map_loading.load(std::memory_order_acquire)) return;
  g_kill_target = actor;
  g_kill_zombies.store(true, std::memory_order_relaxed);
}

// Move speed, a per-player cheat. ClientThink (sub_8230E098) sets ps->speed (client + 148) from g_speed, scales it
// for the speed perks, and then runs Pmove (sub_8269C280), whose steps are the movement step hooked below; the
// server's movement domain is set first. ps->speed is a networked player state field (the netfield entry at
// 0x82979EFC names "speed" at offset 0x94), so the client's prediction gets the server's value with each snapshot.
// For the selected player the server's movement step therefore sees the game's speed scaled by the pick (190, the
// game's g_speed, to 250, 320 or 400). The game writes a fresh value every think, and a value this program wrote
// itself is never scaled again, so the later steps of the same Pmove leave it as it is.
constexpr uint32_t kPsSpeed = 148;
constexpr int kMoveSpeeds[] = {190, 250, 320, 400};
static void ScaleMoveSpeed(uint8_t* base, uint32_t pm) {
  const Actor actor = ActorForPm(base, pm);
  if (!actor || !actor.client || base[pm + 604] == 0) return;
  auto& state = StateForActor(actor);
  const int level = std::clamp(state.options.Effective(Option::Speed), 0, 3);
  const int32_t speed = int32_t(Load32(base, actor.client + kPsSpeed));
  if (level == 0) {
    state.speed_written = INT32_MIN;
  } else if (speed != state.speed_written && speed > 0) {
    const int32_t scaled = int32_t(std::lround(double(speed) * kMoveSpeeds[level] / kMoveSpeeds[0]));
    Store32(base, actor.client + kPsSpeed, uint32_t(scaled));
    state.speed_written = scaled;
  }
}

namespace {
// Test helper (bo2zm_test_goto "x y z", or "log"): scripted test routes need the player at fixed places of a map (for
// example across TranZit's area borders, where the game swaps the area's texture pack). For 0.3 s every movement step
// (the server's and the client's prediction) starts from the given origin with no velocity; from the first command on,
// the origin and yaw are logged every 2 s so a route can be read back from a log.
std::atomic<bool> g_test_move{false};
std::atomic<int64_t> g_test_goto_until{0};
std::mutex g_test_goto_mutex;
Vec3 g_test_goto_at{};

int64_t TestNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void TestMove(uint8_t* base, uint32_t pm) {
  const uint32_t ps = Load32(base, pm + kPmPs);
  if (!ps) return;
  const int64_t now = TestNowNs();
  if (now < g_test_goto_until.load(std::memory_order_relaxed)) {
    Vec3 at;
    { std::lock_guard lock(g_test_goto_mutex); at = g_test_goto_at; }
    StoreVec(base, ps + kPsOrigin, at);
    StoreVec(base, ps + kPsVelocity, Vec3{0.0f, 0.0f, 0.0f});
  }
  static std::atomic<int64_t> next_log{0};
  int64_t due = next_log.load(std::memory_order_relaxed);
  if (now >= due && next_log.compare_exchange_strong(due, now + 2000000000ll, std::memory_order_relaxed)) {
    const Vec3 origin = LoadVec(base, ps + kPsOrigin);
    REXLOG_INFO("[test-pos] at ({:.0f} {:.0f} {:.0f}) yaw {:.0f}", origin.x, origin.y, origin.z,
                LoadFloat(base, ps + kPsViewAngles + 4));
  }
}

}  // namespace

REX_HOOK_RAW(sub_8269AE30) {
  if (!bo2zm::mode::IsZombies()) {
    __imp__sub_8269AE30(ctx, base);
    return;
  }
  if (g_test_move.load(std::memory_order_relaxed)) [[unlikely]] TestMove(base, ctx.r3.u32);
  if (AnyOption(Option::Speed)) [[unlikely]] ScaleMoveSpeed(base, ctx.r3.u32);
  if (!g_hold.load(std::memory_order_relaxed) && !AnyOption(Option::Fly) &&
      !AnyOption(Option::Feather) && !AnyOption(Option::Jump)) {
    __imp__sub_8269AE30(ctx, base); return;
  }
  const uint32_t pm = ctx.r3.u32;
  const Actor actor = ActorForPm(base, pm);
  if (!actor) { __imp__sub_8269AE30(ctx, base); return; }
  auto& state = StateForActor(actor);
  if (g_hold.load(std::memory_order_relaxed)) [[unlikely]] {
    Actor held;
    { std::lock_guard lock(g_hold_mutex); held = g_hold_actor; }
    if (SameActor(held, actor)) { HoldStep(ctx, base, actor); return; }
  }
  const int fly = state.options.Effective(Option::Fly);
  const bool feather = state.options.Effective(Option::Feather);
  const int jump = state.options.Effective(Option::Jump);
  if (fly == bo2zm::cheats::kFlyOff && !feather && jump == 0) [[likely]] {
    __imp__sub_8269AE30(ctx, base);
    return;
  }
  const uint32_t ps = Load32(base, pm + kPmPs);
  if (Load32(base, ps + kPsPmType) != kPmNormal) {
    __imp__sub_8269AE30(ctx, base);
    return;
  }
  if (fly == bo2zm::cheats::kFlyOff) {
    AirStep(ctx, base, pm, ps, jump, feather);
    return;
  }
  const bool collide = state.options.Effective(Option::Collision);
  const uint32_t mask = Load32(base, pm + kPmTraceMask);
  const uint32_t view_height_target = Load32(base, ps + kPsViewHeightTarget);
  const uint32_t view_height = Load32(base, ps + kPsViewHeight);
  Store32(base, ps + kPsPmType, kPmSpectator);
  if (!collide) {
    Store32(base, pm + kPmTraceMask, 0);
  }

  if (fly == bo2zm::cheats::kNoclip) {
    __imp__sub_8269AE30(ctx, base);
  } else {
    const int32_t ms = int32_t(Load32(base, pm + kPmCmdTime)) - int32_t(Load32(base, ps + kPsCommandTime));
    const float dt = std::clamp(float(ms) / 1000.0f, 0.0f, 0.25f);
    Vec3 velocity = UfoVelocity(base, pm, ps, dt);
    // The game's step runs without the stick and buttons (it must not move the player its own way).
    uint8_t stick[2] = {base[pm + kPmForward], base[pm + kPmRight]};
    uint8_t buttons[8];
    std::memcpy(buttons, base + pm + kPmButtons, 8);
    base[pm + kPmForward] = 0;
    base[pm + kPmRight] = 0;
    std::memset(base + pm + kPmButtons, 0, 8);
    const Vec3 origin = LoadVec(base, ps + kPsOrigin);
    StoreVec(base, ps + kPsVelocity, collide ? velocity : Vec3{0.0f, 0.0f, 0.0f});
    __imp__sub_8269AE30(ctx, base);
    base[pm + kPmForward] = stick[0];
    base[pm + kPmRight] = stick[1];
    std::memcpy(base + pm + kPmButtons, buttons, 8);
    if (collide) {
      // The slide move clipped the velocity against what it hit (and the game's friction scaled it): keep the
      // flight's own speed along the clipped direction, nothing into the wall.
      const Vec3 after = LoadVec(base, ps + kPsVelocity);
      const float speed_after = std::sqrt(Dot(after, after));
      const float speed = std::sqrt(Dot(velocity, velocity));
      if (speed_after < 1.0f) {
        velocity = {0.0f, 0.0f, 0.0f};
      } else if (speed > 1.0f && Dot(after, velocity) < 0.9995f * speed_after * speed) {
        const Vec3 d = {after.x / speed_after, after.y / speed_after, after.z / speed_after};
        const float along = std::max(0.0f, Dot(velocity, d));
        velocity = {d.x * along, d.y * along, d.z * along};
      }
    } else {
      StoreVec(base, ps + kPsOrigin,
               {origin.x + velocity.x * dt, origin.y + velocity.y * dt, origin.z + velocity.z * dt});
    }
    StoreVec(base, ps + kPsVelocity, velocity);
  }

  Store32(base, ps + kPsPmType, kPmNormal);
  Store32(base, pm + kPmTraceMask, mask);
  static int logged_fly = bo2zm::cheats::kFlyOff;
  if (fly != logged_fly) {
    logged_fly = fly;
    float kept;
    std::memcpy(&kept, &view_height, 4);
    REXLOG_INFO("[cheats] {}: the spectator step left view height {:.1f} (target {}); kept {:.1f} (target {})",
                FlyName(fly), LoadFloat(base, ps + kPsViewHeight), int32_t(Load32(base, ps + kPsViewHeightTarget)),
                kept, int32_t(view_height_target));
  }
  Store32(base, ps + kPsViewHeightTarget, view_height_target);
  Store32(base, ps + kPsViewHeight, view_height);
}

int bo2zm::cheats::LocalSyncedPlayers(SyncedPlayer* out, int capacity) {
  if (!out || capacity <= 0) return 0;
  std::array<int, kLocalPlayers> controllers;
  {
    std::lock_guard lock(g_actors_mutex);
    controllers = g_controllers;
  }
  int count = 0;
  std::array<bool, kLocalPlayers> seen{};
  for (const int controller : controllers) {
    if (controller < 0 || controller >= kLocalPlayers || seen[controller] || count >= capacity) continue;
    seen[controller] = true;
    out[count].controller = controller;
    for (int i = 0; i < detail::kSyncedCount; ++i) {
      out[count].values[i] = g_players[controller].options.Effective(detail::kSyncedOptions[i]);
    }
    ++count;
  }
  if (count == 0) {
    // No view is bound to a controller yet: the menu's picks are the primary controller's (StateForLocal).
    const int controller = g_primary_controller.load(std::memory_order_relaxed);
    out[0].controller = controller;
    for (int i = 0; i < detail::kSyncedCount; ++i) {
      out[0].values[i] = g_players[controller].options.Effective(detail::kSyncedOptions[i]);
    }
    count = 1;
  }
  return count;
}

bool bo2zm::cheats::SetRemotePlayer(const uint8_t* base, uint64_t xuid, uint32_t sequence, const int* values,
                                    int* client, bool* changed) {
  if (client) *client = -1;
  if (changed) *changed = false;
  if (!base || !xuid || !values) return false;
  int remote = -1;
  for (int i = 0; i < kRemotePlayers && remote < 0; ++i) {
    if (g_remote_slots[i].xuid == xuid) remote = i;
  }
  bool fresh = false;
  if (remote < 0) {
    for (int i = 0; i < kRemotePlayers && remote < 0; ++i) {
      if (!g_remote_slots[i].xuid) remote = i;
    }
    if (remote < 0) {
      REXLOG_WARN("[cheat sync] remote player {:016X}: no free slot (all {} in use); ignored", xuid, kRemotePlayers);
      return false;
    }
    fresh = true;
    auto& slot = g_remote_slots[remote];
    slot = {};
    slot.xuid = xuid;
    slot.since = std::chrono::steady_clock::now();
    g_remote_players[remote].options.ResetAll();
  }
  auto& slot = g_remote_slots[remote];
  if (!fresh && int32_t(sequence - slot.sequence) < 0) return false;  // older than the one already taken
  const bool repeat = !fresh && sequence == slot.sequence;
  slot.sequence = sequence;
  auto& options = g_remote_players[remote].options;
  if (!repeat) {
    for (int i = 0; i < detail::kSyncedCount; ++i) {
      const auto option = detail::kSyncedOptions[i];
      if (options.Effective(option) != values[i] && changed) *changed = true;
      options.Direct(option, values[i]);
    }
  }
  uint32_t entity = 0, client_address = 0;
  const int number = FindRemoteClient(base, xuid, entity, client_address);
  if (client) *client = number;
  return true;
}

bool bo2zm::cheats::JoinedRemoteGame() { return g_joined.load(std::memory_order_acquire); }
bool bo2zm::cheats::LocalServer(const uint8_t* base) { return base && LocalServerRunning(base); }

int bo2zm::cheats::RunRemoteCommand(const uint8_t* base, uint64_t xuid, std::string_view name,
                                    std::string_view argument, bool pack_a_punch, std::string& why) {
  why.clear();
  const auto refuse = [&](std::string text) {
    why = std::move(text);
    return 1;
  };
  if (!base || !bo2zm::mode::IsZombies()) return refuse("not a Zombies match");
  if (g_map_loading.load(std::memory_order_acquire)) return refuse("a map is loading");
  Actor actor;
  for (int remote = 0; remote < kRemotePlayers && !actor; ++remote) {
    const Actor candidate = RemoteActor(remote);
    if (candidate && candidate.xuid == xuid && CurrentActor(base, candidate)) actor = candidate;
  }
  if (!actor) return refuse("this player is not in the host's match (yet)");
  if (!StateForActor(actor).options.Effective(Option::Cheats)) return refuse("CHEATS is off for this player");
  const auto lower = [](std::string_view text, std::string_view allowed, size_t most) {
    return !text.empty() && text.size() <= most && text.find_first_not_of(allowed) == std::string_view::npos;
  };
  constexpr std::string_view kName = "abcdefghijklmnopqrstuvwxyz_";
  constexpr std::string_view kWeapon = "abcdefghijklmnopqrstuvwxyz0123456789_";
  if (name == "bo2zm_perk") {
    if (!lower(argument, kName, 40)) return refuse("not a perk name");
    QueuePlayer(actor, PlayerAction::Perk, argument);
  } else if (name == "bo2zm_remove_perks") {
    QueuePlayer(actor, PlayerAction::RemovePerks);
  } else if (name == "bo2zm_give_weapon") {
    if (!lower(argument, kWeapon, 48)) return refuse("not a weapon name");
    if (!MenuGun(argument)) return refuse("this map has no such gun");
    QueuePlayer(actor, PlayerAction::Gun, argument, pack_a_punch);
  } else if (name == "bo2zm_pap_weapon") {
    QueuePlayer(actor, PlayerAction::Pap);
  } else if (name == "bo2zm_max_points") {
    QueuePlayer(actor, PlayerAction::Points);
  } else if (name == "bo2zm_powerup") {
    if (!lower(argument, kName, 32)) return refuse("not a power-up name");
    QueuePlayer(actor, PlayerAction::Powerup, argument);
  } else if (name == "bo2zm_kill_zombies") {
    std::lock_guard lock(g_script_mutex);
    g_kill_target = actor;
    g_kill_zombies.store(true, std::memory_order_relaxed);
  } else if (name == "bo2zm_doors" || name == "bo2zm_power" || name == "bo2zm_debris") {
    if ((name == "bo2zm_doors" && argument != "open" && argument != "close") ||
        (name == "bo2zm_power" && argument != "on" && argument != "off") ||
        (name == "bo2zm_debris" && argument != "remove" && argument != "restore")) return refuse("not a choice");
    std::lock_guard lock(g_script_mutex);
    g_world_target = actor;
    if (name == "bo2zm_doors") g_doors_request = argument == "open" ? 1 : 2;
    else if (name == "bo2zm_power") g_power_request = argument == "on" ? 1 : 2;
    else g_debris_request = argument == "remove" ? 1 : 2;
    g_world_waiting.store(true, std::memory_order_relaxed);
  } else if (name == "bo2zm_goto_round") {
    if (!lower(argument, "0123456789", 3)) return refuse("not a round");
    const int round = std::atoi(std::string(argument).c_str());
    if (round < 1 || round > 255) return refuse("not a round");
    std::lock_guard lock(g_script_mutex);
    g_round_target = actor;
    g_round_request = round;
    g_script_waiting.store(true, std::memory_order_relaxed);
  } else if (name == "bo2zm_zm_nospawn") {
    if (argument != "0" && argument != "1") return refuse("not a choice");
    g_stop_spawns.store(argument == "1", std::memory_order_relaxed);
  } else if (name == "bo2zm_hold" || name == "bo2zm_release") {
    const size_t space = argument.find(' ');
    const WorldDvar* dvar = space == std::string_view::npos ? nullptr : FindWorldDvar(argument.substr(0, space));
    if (!dvar || !WorldValue(*dvar, argument.substr(space + 1))) return refuse("not a world setting");
    MenuHold(name == "bo2zm_hold", argument);
  } else {
    return refuse("not a command this host takes");
  }
  return 0;
}

bool bo2zm::cheats::CatalogText(std::string& text, uint32_t& revision) {
  std::lock_guard lock(g_catalog_mutex);
  text = g_catalog_text;
  revision = g_catalog_revision;
  return !text.empty();
}

void bo2zm::cheats::ApplyCatalog(PPCContext& ctx, uint8_t* base, std::string_view text) {
  // "G<guns>\nP<perks>\nW<power-ups>", each part checked against the menu's character set.
  std::string_view parts[3];
  const char tags[3] = {'G', 'P', 'W'};
  for (int i = 0; i < 3; ++i) {
    const size_t end = std::min(text.find('\n'), text.size());
    const std::string_view line = text.substr(0, end);
    if (line.empty() || line[0] != tags[i]) {
      REXLOG_WARN("[cheats] the host's catalog is malformed; ignored");
      return;
    }
    parts[i] = line.substr(1);
    if (parts[i].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_, ") !=
        std::string_view::npos) {
      REXLOG_WARN("[cheats] the host's catalog has unexpected characters; ignored");
      return;
    }
    text = end < text.size() ? text.substr(end + 1) : std::string_view();
  }
  ShowCatalog(ctx, base, parts[0], parts[1], parts[2]);
}

std::string bo2zm::cheats::WorldStateText() {
  std::string text;
  {
    std::lock_guard lock(g_held_mutex);
    for (const auto& world : kWorldDvars) {
      for (const Held& h : g_held) {
        if (h.name == world.name && h.menu && !h.release && WorldValue(world, h.value)) {
          text += std::string(world.name) + "=" + h.value + ";";
        }
      }
    }
  }
  if (g_stop_spawns.load(std::memory_order_relaxed)) text += "zm_nospawn=1;";
  return text;
}

void bo2zm::cheats::ApplyWorldState(std::string_view text) {
  bool spawns_stopped = false;
  std::lock_guard lock(g_held_mutex);
  for (const auto& world : kWorldDvars) {
    std::string_view value;
    for (size_t at = 0; at < text.size();) {
      const size_t end = std::min(text.find(';', at), text.size());
      const std::string_view pair = text.substr(at, end - at);
      const size_t equals = pair.find('=');
      if (equals != std::string_view::npos && pair.substr(0, equals) == world.name &&
          WorldValue(world, pair.substr(equals + 1))) value = pair.substr(equals + 1);
      if (pair == "zm_nospawn=1") spawns_stopped = true;
      at = end + 1;
    }
    Held* entry = nullptr;
    for (Held& h : g_held) if (h.name == world.name) entry = &h;
    if (!value.empty()) {
      if (!entry || entry->value != value || entry->release || !entry->mirror) {
        HoldLocked(std::string(world.name), std::string(value), true);
        for (Held& h : g_held) if (h.name == world.name) { h.mirror = true; h.menu = false; }
        REXLOG_INFO("[cheats] the host holds {} at {}", world.name, value);
      }
      Show(std::string(world.name), std::string(value));
    } else if (entry && (entry->mirror || entry->menu) && !entry->release) {
      HoldLocked(std::string(world.name), std::string(), false);
      Show(std::string(world.name), std::string(*world.values.begin()));
      REXLOG_INFO("[cheats] the host gives {} back to the game", world.name);
    }
  }
  g_stop_spawns.store(spawns_stopped, std::memory_order_relaxed);
  g_held_dirty.store(true, std::memory_order_relaxed);
}

int bo2zm::cheats::TakeRemoteBlasts(RemoteBlastHit* out, int capacity) {
  std::lock_guard lock(g_blasts_mutex);
  int count = 0;
  for (const auto& blast : g_remote_blasts) {
    if (count >= capacity) break;
    out[count++] = {blast.xuid, {blast.at.x, blast.at.y, blast.at.z}};
  }
  g_remote_blasts.clear();
  return count;
}

void bo2zm::cheats::PlayRemoteBlast(const float at[3]) {
  const Actor actor = PrimaryActor();
  if (!actor || !g_joined.load(std::memory_order_acquire)) return;
  std::lock_guard lock(g_blasts_mutex);
  g_blasts_waiting.store(true, std::memory_order_relaxed);
  if (g_blasts.size() < 16) g_blasts.push_back({actor, {at[0], at[1], at[2]}});
}

void bo2zm::cheats::TestGoto(std::string_view argument) {
  g_test_move.store(true, std::memory_order_relaxed);
  float x = 0, y = 0, z = 0;
  const std::string text(argument);
  if (std::sscanf(text.c_str(), "%f %f %f", &x, &y, &z) != 3) {
    REXLOG_INFO("[test-pos] logging the player's origin (bo2zm_test_goto {})", text);
    return;
  }
  { std::lock_guard lock(g_test_goto_mutex); g_test_goto_at = Vec3{x, y, z}; }
  g_test_goto_until.store(TestNowNs() + 300000000ll, std::memory_order_relaxed);
  REXLOG_INFO("[test-pos] goto ({:.0f} {:.0f} {:.0f})", x, y, z);
}
