// Graphics presets (docs/research/06-performance-program.md): bo2zm_preset = lowest, very_low, low, medium, high or
// custom.
//
// A preset is a level for each graphics option (kOptions: texture detail, model detail, draw distance, shadows, dynamic
// lights, screen effects, decals, corpses and debris); each level sets BO2's own quality dvars, still registered in the
// TU18 binary although the 360 has no menu for them, or a renderer option. An option's top level leaves the game's own
// values. "custom" uses the levels the player picked one by one in Options > Graphics (saved as gfx_<option>): changing
// an option makes the preset custom, starting from the levels of the preset in use, and picking a preset sets every
// option to its levels. Lowest goes one step further than very low on textures, draw distance and corpses and debris.
// High is the game's own values for every option, as the Xbox 360 runs it (every option at its top level). The
// preset is applied through the game's console on the main thread (bo2zm_autoexec.cpp runs the commands) about two
// seconds after each map load, again whenever it changes (settings menu or the button combination), and it is checked
// every few seconds so that a level script resetting a dvar does not undo it. Each dvar in the table was checked on the
// PC: it exists, the "set" takes (read back from the dvar after the command), and it changes the image or the cost.
//
// The Graphics Settings menu has a GRAPHICS PRESET option (app/src/ui/menus.lua): it shows the game dvar
// bo2zm_preset_ui (0-5, kept in sync from here) and changes the preset with the console command "bo2zm_preset N"
// (bo2zm_commands.cpp); each option shows bo2zm_gfx_<option>_ui and changes with "bo2zm_gfx_<option> N". The chosen
// preset is saved in <user folder>/bo2zm_settings.txt (bo2zm_settings.cpp) and loaded at start-up, unless bo2zm.toml,
// the environment or the command line set bo2zm_preset (any value: a test toml's preset always wins). Very low is the
// default.
//
// In menus or in game: hold Back (Minus on the Switch) and press Up for the next higher preset or Down for the next
// lower one (lowest to high, wrapping around; from custom, Up goes to lowest and Down to high).
// A note shows the new preset for a couple of seconds (the output pass draws it; with SHOW PRESET enabled the preset
// is in the corner the whole time anyway). While Back is held the game does not see Up or Down, so the combination
// never reaches it; Back alone still shows the scoreboard as always.
//
// The game's dvars, from TU18 default_mp.xex:
//   - Registration: sub_82498550 bool(name, value, flags, description), sub_824985D0 int(name, value, min, max,
//     flags, description), sub_82498668 float(name, value f1, min f2, max f3, flags r7, description r8),
//     sub_82498880 enum(name, value list, value, flags, description). Each returns the dvar. Call sites read with
//     tools: e.g. r_lodScaleRigid at 0x828B120C, sm_sunEnable at 0x828B21C4, sm_maxLights at 0x828B2268.
//   - dvar layout: name at +0x0, flags at +0xC, type at +0x10, current value at +0x18 (Dvar_SetVariant around
//     0x82497FE4: lwz 0xC, lwz 0x10, ld 0x18/0x20; the cheat check sub_82497E78 reads flags at +0xC).
//   - Flag 0x80 is the cheat flag: sub_82497E78 refuses a change from the console or a script while sv_cheats is
//     off and prints "%s is cheat protected." (0x82497EF0-0x82497F40). Many quality dvars carry it (r_zfar,
//     r_dof_enable, r_texFilterMipBias, fx_marks_draw, ...), so it is cleared on our dvars only while our own
//     commands run, and put back right after. Flags 0x40 (read only) and 0x10 (write protected) are left alone.
//   - Flag 0x1 makes the game write the dvar to its config as "seta" (sub_824152A8, 0x824152D0-0x8241530C). It is
//     cleared on the dvars this file manages when they register, so a preset never ends up in the saved config and
//     an option's top level always means the game's own values.

#include "bo2zm_preset.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "bo2zm_autoexec.h"
#include "bo2zm_cheats.h"
#include "bo2zm_dvars.h"
#include "bo2zm_game.h"
#include "bo2zm_mode.h"
#include "bo2zm_settings.h"
#include "bo2zm_switch.h"
#include "native/render/bo2zm_native_capture.h"

REXCVAR_DEFINE_BOOL(bo2zm_far_skip, true, "Graphics",
                    "The world's static models wholly past the draw distance (r_zfar, as the preset sets it) are not "
                    "sent to the GPU, which would clip them anyway. false = the game sends them (to compare)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(bo2zm_far_simple_size, 0.06, "Graphics",
                      "FAR OBJECTS SIMPLE: a static model wholly past the draw distance is still drawn, in its "
                      "simplest version, when it reaches at least this share of its distance (0.06: about 60 pixels "
                      "from its middle on a 1280x720 screen); smaller ones are dropped. 0 = keep them all")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_model_detail_native, true, "Graphics",
                    "Model detail (the presets' MODEL DETAIL) through the game's version choice: models switch to "
                    "simpler versions sooner, and none is hidden that the game would draw (the world's static models "
                    "were, with r_lodScaleRigid above 1: Buried's cave walls). false = the two r_lodScale dvars, as "
                    "before (to compare)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_model_detail_reach, true, "Graphics",
                    "Model detail below the game's own: a long model's distance (one reaching 256 units or more from "
                    "its origin) is scaled from its nearest possible point (its distance less its reach), so a far "
                    "origin does not make it simple right in front of the player (Nuketown's truck). false = from "
                    "its origin, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_diag_model_detail, "", "Graphics",
                      "Diagnostics: models whose name contains this text log their version choice (the ramp and plain "
                      "distances, their versions' limits, the game's own pick and the preset's) when it changes, at "
                      "most once a second each. Empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Default quality. The Switch keeps its own (very_low, split screen capped at lowest, model scale 2); every other
// build (the Mac) defaults to kDefaultPreset: change this ONE line to "high" for the high default. The 4p Town matrix
// held 60 fps at both.
#if defined(__SWITCH__)
inline constexpr const char* kDefaultPreset = "very_low";
inline constexpr const char* kDefaultSplitscreenPreset = "lowest";
inline constexpr double kDefaultSplitscreenModelScale = 2.0;
#else
inline constexpr const char* kDefaultPreset = "medium";  // "medium" or "high"
inline constexpr const char* kDefaultSplitscreenPreset = "same";
inline constexpr double kDefaultSplitscreenModelScale = 1.0;
#endif
REXCVAR_DEFINE_STRING(bo2zm_preset, kDefaultPreset, "Graphics",
                      "Graphics preset: lowest, very_low, low, medium, high or custom (high = the game's own "
                      "settings, as the Xbox 360 runs it; custom = the options picked one by one in Options > "
                      "Graphics). Lower presets trade model detail, draw distance, shadows, screen "
                      "effects and texture detail for frame rate. "
                      "Applied when a map loads and right away when changed. Anywhere: hold Back (Minus) and press "
                      "Up or Down")
    .allowed({"lowest", "very_low", "low", "medium", "high", "custom"});

REXCVAR_DEFINE_INT32(bo2zm_preset_test_s, 0, "BO2",
                     "Test helper: once in a match, step to the next lower preset every N seconds (high, medium, "
                     "low, very_low, lowest, high, ...), logged as '[preset]'. 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_preset_on_screen, true, "BO2",
                    "The current preset stays in the top-right corner the whole time, in menus and in game, and "
                    "changes as soon as the preset changes (Graphics menu, button combination). false = only for a "
                    "couple of seconds after the button combination changes it. SHOW PRESET in the Graphics menu "
                    "(saved as show_preset)");

REXCVAR_DEFINE_STRING(bo2zm_test_console, "", "BO2",
                      "Test helper: console commands at seconds after the match loads, \"S:cmd;cmd|S:cmd\"; each "
                      "dvar named in a set command is read back into the log. Three commands are ours: \"png NAME\" "
                      "saves captures/NAME.png, \"rdc\" captures the next frame under RenderDoc (PC) and \"preset NAME\" "
                      "sets bo2zm_preset")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The campaign's front end. Its menus sit over a 3D level of their own (the map "frontend"), and that level is
// heavier than most missions: on a Switch Lite at the stock 307.2 MHz, very low gives about 2,700 draws and 520,000
// triangles a frame (every opaque mesh twice: a depth prepass, then the colour pass, all in the one 880x720 view),
// 70 ms of GPU time and 12 FPS. The campaign program tells the preset when that level is loaded (SetFrontEnd); while
// it is, every option is capped at bo2zm_front_end_preset's level (lowest: the shortest draw distance too) and FAR
// OBJECTS is SIMPLE with no model dropped for its size, so the models past the draw distance stay, in their simplest
// version, and nothing in the room disappears (the draw distance alone, with FAR OBJECTS on HIDE, removed scenery
// there). The renderer also skips the scene's depth prepass there (bo2zm_native_front_end_no_prepass) and draws the
// scene again only when its view changes (bo2zm_native_front_end_reuse); bo2zm_front_end_fps_cap can cap the frame
// rate. A mission gets the player's preset back once mapname names it.
#if defined(__SWITCH__)
constexpr const char* kFrontEndPresetDefault = "lowest";
#else
constexpr const char* kFrontEndPresetDefault = "same";
#endif
REXCVAR_DEFINE_STRING(bo2zm_front_end_preset, kFrontEndPresetDefault, "Graphics",
                      "Campaign: the highest level the front-end menus' 3D scene gets for each option, as this "
                      "preset's (the player's applies when it is lower), with FAR OBJECTS on SIMPLE and every model "
                      "kept. \"same\" = the player's preset (the PC's default; lowest on the Switch). Missions "
                      "always get the player's preset")
    .allowed({"same", "lowest", "very_low", "low", "medium", "high"});
REXCVAR_DEFINE_INT32(bo2zm_front_end_fps_cap, 0, "Graphics",
                     "Campaign: the frame rate cap in the front-end menus (30 = a steady 30 rather than an uneven "
                     "20-something while the scene is drawn every frame). 0 = none")
    .range(0, 60);
REXCVAR_DEFINE_INT32(bo2zm_front_end_test_s, 0, "Graphics",
                     "Test helper: in the campaign's front end, the player's preset and the front-end settings (with "
                     "the prepass skip and the frame rate cap) take turns every N seconds, and each period's frame "
                     "rate is logged as '[preset] front end test'. 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Split screen. The 360 game itself changes no render, LOD or draw distance dvar for split screen (its
// default_splitscreen.cfg only scales the HUD and compass; its LOD choice, sub_828AF1E8, uses the view's field of view
// and r_lodScale*, with no term for the view's size), so with four players every view costs what the one view did:
// about 1,400 draws and the game's whole scene preparation, four times a frame. Two things are scaled here instead,
// once the game reports more than one local view (splitscreen_playerCount):
//  - every option is capped at bo2zm_splitscreen_preset's level (the player's applies when it is lower);
//  - the model detail factor is multiplied by bo2zm_splitscreen_model_scale with three or four views (each view is
//    half as wide and half as tall, so a model at twice the distance covers the pixels it did in one view) and by its
//    square root with two (half as tall only). The version a model gets therefore matches its size on screen, as in
//    one view.
REXCVAR_DEFINE_STRING(bo2zm_splitscreen_preset, kDefaultSplitscreenPreset, "Graphics",
                      "Split screen: the highest level each option gets while more than one player plays on this "
                      "machine, as this preset's (the player's applies when it is lower). \"same\" = no cap")
    .allowed({"same", "lowest", "very_low", "low", "medium", "high"});
REXCVAR_DEFINE_DOUBLE(bo2zm_splitscreen_model_scale, kDefaultSplitscreenModelScale, "Graphics",
                      "Split screen: the model detail factor is multiplied by this with three or four views and by "
                      "its square root with two, so models get the version their size on screen calls for. 1 = as "
                      "with one view")
    .range(1.0, 4.0);

namespace bo2zm::preset {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPresets = 6;
constexpr const char* kNames[kPresets] = {"lowest", "very_low", "low", "medium", "high", "custom"};
constexpr const char* kNotes[kPresets] = {"PRESET: LOWEST", "PRESET: VERY LOW", "PRESET: LOW", "PRESET: MEDIUM",
                                          "PRESET: HIGH",   "PRESET: CUSTOM"};
constexpr int kDefault = 1;  // very low
constexpr int kHighest = 4;  // high
constexpr int kCustom = 5;
constexpr int kStepped = 5;  // the button combination and the test step through lowest..high

constexpr uint32_t kDvarFlags = 0xC;
constexpr uint32_t kDvarCurrent = 0x18;
constexpr uint32_t kFlagFile = 0x1;
constexpr uint32_t kFlagTrap = 0x80;

// The game's entities: player 0's client pointer is non-null once a match is loaded (bo2zm_autoexec.cpp).
constexpr uint32_t kGEntities = BO2_GAME_ADDR(0x833D0640, 0x82F91DF8);
constexpr uint32_t kEntClient = BO2_GAME_ADDR(340, 344);

enum Kind { kBool = 0, kWhole = 1, kReal = 2, kEnum = 3 };

// The graphics options (Options > Graphics), each with its levels from the lowest to the game's own.
enum OptionId { kTexture, kModel, kDistance, kFar, kShadows, kLights, kEffects, kDecals, kBodies, kNumOptions };
struct Option {
  const char* name;  // gfx_<name>: the menu's setting, the console command and the settings file's key
  int levels;
  const char* level_names[5];
};
constexpr Option kOptions[kNumOptions] = {
    {"texture", 4, {"lowest", "low", "medium", "high"}},
    {"model", 4, {"low", "medium", "high", "ultra"}},
    {"distance", 5, {"shortest", "short", "medium", "far", "full"}},
    {"far", 2, {"hide", "simple"}},  // what lies past the draw distance (kFarSimple below)
    {"shadows", 4, {"off", "low", "medium", "high"}},
    {"lights", 4, {"off", "low", "medium", "high"}},
    {"effects", 2, {"off", "on"}},
    {"decals", 4, {"off", "near", "medium", "far"}},
    {"bodies", 4, {"fewest", "few", "some", "all"}},
};

// Each preset's level of each option (custom has its own, g_custom).
constexpr int kPresetLevels[kStepped][kNumOptions] = {
    // texture model distance far shadows lights effects decals bodies
    {0, 0, 0, 0, 0, 0, 0, 0, 0},  // lowest
    {1, 0, 1, 0, 0, 0, 0, 0, 1},  // very low
    {2, 1, 2, 0, 1, 1, 0, 2, 2},  // low
    {3, 2, 3, 0, 2, 2, 1, 3, 3},  // medium
    {3, 3, 4, 0, 3, 3, 1, 3, 3},  // high: every option at its top level, the game's own values
};

// The dvars, each with a value per level of its option; nullptr = the game's own value.
struct Setting {
  const char* dvar;
  OptionId option;
  const char* values[5];
};
constexpr Setting kSettings[] = {
    // Model detail: the LOD switch distances are divided by this scale (1 = the game's, 4 = the most it takes).
    // With bo2zm_model_detail_native (on) these two stay at the game's own value and kModelFactors below does the
    // same through the version choice instead (see there).
    {"r_lodScaleRigid", kModel, {"4", "2", "1.5", nullptr}},
    {"r_lodScaleSkinned", kModel, {"4", "2", "1.5", nullptr}},
    // Draw distance (0 = the map's own far plane). Town's fog hides everything past ~1500 units.
    {"r_zfar", kDistance, {"900", "1200", "1500", "2500", nullptr}},
    // Sun and spot light shadows.
    {"sm_sunEnable", kShadows, {"0", nullptr, nullptr, nullptr}},
    {"sm_spotEnable", kShadows, {"0", "0", nullptr, nullptr}},
    {"sm_maxLights", kShadows, {"0", "1", "2", nullptr}},
    // Screen effects.
    {"r_dof_enable", kEffects, {"0", nullptr}},
    {"r_flame_allowed", kEffects, {"0", nullptr}},
    {"r_waterSheetingFX_allowed", kEffects, {"0", nullptr}},
    {"fx_drawClouds", kEffects, {"0", nullptr}},
    // Decals.
    {"fx_marks_draw", kDecals, {"0", nullptr, nullptr, nullptr}},
    {"fx_marks_range", kDecals, {"300", "300", "800", nullptr}},
    // Dynamic lights.
    {"r_dlightLimit", kLights, {"0", "1", "2", nullptr}},
    // Corpses and spawned dynamic entities.
    {"ai_corpseCount", kBodies, {"1", "2", "4", nullptr}},
    {"dynEnt_spawnedLimit", kBodies, {"5", "10", "25", nullptr}},
};
constexpr size_t kNumSettings = sizeof(kSettings) / sizeof(kSettings[0]);

// Model detail without hiding models. r_lodScaleRigid and r_lodScaleSkinned multiply the distance a model's version is
// chosen with (the LOD ramp, sub_828AF1E8 -> sub_828CC170 and sub_828CC2A8: distance x scale + bias), but the world's
// static models are also culled with that scaled distance: sub_828CAA78 (and its siblings for the other views) skips an
// instance when distance x scale + bias reaches the instance's own cull distance (its first float), before any version
// is chosen. At 4 (very low) the static models disappeared at a quarter of their distance: on the console, Buried's
// cave walls vanished and the sky showed through them (one tier up, at 2, they stayed). The game's version choice
// itself, sub_824DE0F8(model r3, ramp distance f1, plain distance f2, r6), already falls back to the plain distance
// when only the scale puts a model past its last version.
//
// So the presets leave both dvars at the game's own value (every cull is the game's) and apply the model detail here,
// where only the version is chosen: the ramp distance is multiplied by the factor of the model's ramp (its byte at
// +7: 0 rigid, 1 skinned) and compared with its versions' limits (floats at +40, 28 bytes apart; the count is the
// short at +196). Past the last version, a model the game draws keeps its last, simplest version; one the game itself
// puts past its versions gets the game's own answer. bo2zm_model_detail_native = false: the two dvars, as before.
constexpr float kModelFactors[4] = {4.0f, 2.0f, 1.5f, 1.0f};  // per model detail level (low ... ultra)
std::atomic<float> g_model_factor{1.0f};
// Models kept at their simplest version (the factor alone would have dropped them), by their plain distance divided by
// their scale: under 600, under 1,200, under 2,500, beyond. Logged every 10 s (EveryFrame). On the PC, Town at very low
// keeps about 105 a frame, half of them past the 1,200-unit far plane.
constexpr float kKeptLimits[3] = {600.0f, 1200.0f, 2500.0f};
std::array<std::atomic<uint32_t>, 4> g_kept{};
// Of those, the ones wholly past the far plane are still dropped, as the factor did: they cannot be seen. Wholly:
// the plain distance divided by the scale, less the model's radius (the float at +168, sub_824DC938), beyond r_zfar.
// The static models' scale is at least 1 (sub_828CAA78 raises it to 1.0, the float at 0x82000DB0), so that is never
// nearer than the truth. r_zfar 0 (the map's own far plane) drops nothing. Only models the factor alone would have
// dropped are ever dropped, so nothing is hidden that was drawn before.
std::atomic<float> g_far_plane{0.0f};  // r_zfar as the preset set it
std::atomic<uint32_t> g_dropped{0};
std::atomic<uint32_t> g_held_back{0};  // the simplest version held back (the game would use a better one)
// bo2zm_far_skip: every static model wholly past the far plane is dropped, not only the ones above. The GPU would clip
// all of them; the game still sent them. Only from the world's static model passes, which raise every instance's scale
// to at least 1 before the version choice (the return addresses of their calls to sub_824DE0F8): sub_828CAA78,
// sub_828CCEF0, sub_828CD240 and sub_828CDAA8. The other callers (sub_824CF2C8, the entities' models; sub_828F7D00) can
// pass a scale below 1, which would make a model look farther than it is.
constexpr uint32_t kStaticModelCalls[] = {BO2_GAME_ADDR(0x828CAC60, 0x826C67A8), BO2_GAME_ADDR(0x828CD098, 0x826D0DC8),
                                         BO2_GAME_ADDR(0x828CD398, 0x826D10C8), BO2_GAME_ADDR(0x828CDC6C, 0x826D17AC)};
std::atomic<uint32_t> g_far_skipped{0};

// FAR OBJECTS: what lies past the draw distance. HIDE is the draw distance as it always was: r_zfar both moves the
// projection's far plane in (the view setup's sub_828A3AB8, r_zfar or else the map's own, the float at 0x8479DA38) and
// adds a sixth plane to the game's cull (sub_828A4088), so every surface and model past it is cut or never sent, and
// the sky shows where a cave wall or a building stood (Buried's caves, the witch's mansion). SIMPLE leaves the far
// plane at the map's own (r_zfar 0, so nothing is cut) and keeps the draw distance for the world's static models only:
// one wholly past it is dropped when it is small on screen (reaching less than bo2zm_far_simple_size of its distance)
// and otherwise drawn in its simplest version, the coarse shape old games show far away. The level geometry itself (the
// map's surfaces, not models) and the entities are drawn as far as the map draws them.
constexpr int kFarSimple = 1;
std::atomic<bool> g_far_simple{false};
std::atomic<float> g_far_simple_size{0.06f};  // bo2zm_far_simple_size, or 0 in the campaign's front end (Apply)
std::atomic<uint32_t> g_far_kept{0};     // SIMPLE: static models past the draw distance drawn in their simplest version
std::atomic<uint32_t> g_far_dropped{0};  // SIMPLE: static models past the draw distance dropped as small
constexpr uint32_t kMapFarPlane = BO2_GAME_ADDR(0x8479DA38, 0x841388F8);  // the map's own far plane (sub_828A3AB8: lis -31622, -9792, +120)

bool ModelDetailNative() {
  static const bool native = REXCVAR_GET(bo2zm_model_detail_native);
  return native;
}

// The game sets these back to 1 every frame (sub_828A84F0, below), so the preset's value is written again right
// after it.
constexpr const char* kForced[] = {"sm_sunEnable", "sm_spotEnable"};

// Texture detail: textures with mips start this many levels lower (bo2zm_native_diag_min_mip's mechanism), per level.
constexpr uint32_t kMipsMinus[4] = {3, 2, 1, 0};

struct DvarRecord {
  uint32_t pointer = 0;
  int type = kBool;
};

// A dvar's current value: integer (bool, int, enum) or float.
struct Value {
  int type = kBool;
  int32_t whole = 0;
  float real = 0.0f;

  bool operator==(const Value& o) const {
    if (type == kReal) {
      return std::fabs(real - o.real) <= 1e-4f * std::max(1.0f, std::fabs(real));
    }
    return whole == o.whole;
  }
  std::string Text() const {
    char text[32];
    if (type == kReal) {
      std::snprintf(text, sizeof(text), "%.6g", double(real));
    } else {
      std::snprintf(text, sizeof(text), "%d", whole);
    }
    return text;
  }
};

// The dvars the table manages, in table order: where they are, the game's own value and what we wrote last.
struct Managed {
  DvarRecord reg_index;
  std::optional<Value> original;
  std::optional<Value> written;
  bool warned = false;  // "not registered" logged once
};

std::array<Managed, kNumSettings> g_managed;

std::atomic<int> g_index{kDefault};      // the preset bo2zm_preset names
std::array<std::atomic<int>, kNumOptions> g_custom{};  // custom's levels, from the settings file (LoadCustom)
std::atomic<bool> g_custom_dirty{false};  // custom's levels changed: save them
std::atomic<bool> g_pending{false};   // apply on the next frame (the preset changed)
std::atomic<bool> g_in_match{false};  // player 0 is in a match
std::atomic<bool> g_front_end{false};  // the campaign's front-end level is loaded (SetFrontEnd)
std::atomic<int> g_front_end_step{-1};  // bo2zm_front_end_test_s: 0 the player's preset, 1 the front-end settings
std::atomic<int> g_request{0};         // from the combination: +1 higher, -1 lower
std::atomic<uint32_t> g_mips_minus{0};
std::array<std::atomic<int32_t>, kNumSettings> g_forced{};  // kForced: 1 + the value to keep, 0 = the game's

std::atomic<uint32_t> g_note[4] = {};
std::atomic<int64_t> g_note_until{0};  // steady-clock ns until which the note shows
std::atomic<uint32_t> g_banner[8] = {};
std::atomic<int64_t> g_banner_from{0};   // steady-clock ns from which the banner shows
std::atomic<int64_t> g_banner_until{0};  // steady-clock ns until which the banner shows
std::mutex g_map_mutex;
std::string g_map;  // the last bo2zm_profile_map words (map, game type, start location), under g_map_mutex

int64_t Ns(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

int IndexOf(std::string_view label) {
  for (int i = 0; i < kPresets; ++i) {
    if (label == kNames[i]) {
      return i;
    }
  }
  return kDefault;
}

// What the campaign's front end changes (see bo2zm_front_end_preset): the preset no option goes above (-1: none, and
// FAR OBJECTS stays the player's).
int FrontEndCap() {
  const int step = g_front_end_step.load(std::memory_order_relaxed);
  if (!g_front_end.load(std::memory_order_relaxed) || step == 0) {
    return -1;
  }
  const std::string& name = REXCVAR_GET(bo2zm_front_end_preset);
  return name == "same" ? -1 : std::min(IndexOf(name), kHighest);
}

// An option's level for a preset: the table's, or custom's.
int LevelOf(int preset, int option) {
  return preset == kCustom ? g_custom[option].load(std::memory_order_relaxed) : kPresetLevels[preset][option];
}

// Custom's levels from the settings file ("gfx_<option> = <level name>"); an option not saved starts at very low's.
void LoadCustom() {
  for (int o = 0; o < kNumOptions; ++o) {
    int level = kPresetLevels[kDefault][o];
    if (const auto value = bo2zm::settings::Get(std::string("gfx_") + kOptions[o].name)) {
      for (int l = 0; l < kOptions[o].levels; ++l) {
        if (*value == kOptions[o].level_names[l]) {
          level = l;
        }
      }
    }
    g_custom[o].store(level, std::memory_order_relaxed);
  }
}

void SaveCustom() {
  for (int o = 0; o < kNumOptions; ++o) {
    bo2zm::settings::Set(std::string("gfx_") + kOptions[o].name,
                         kOptions[o].level_names[g_custom[o].load(std::memory_order_relaxed)]);
  }
}

int g_saved = -1;  // the preset written to (or read from) the settings file

// Map profiles (Options > Graphics): a location's own preset and custom levels, used there instead of the global ones
// (g_index, g_custom) once the player turns its profile on. The menu Lua says which location is loading
// (bo2zm_profile_map, from the loading screen: the map, its game type and its start location). Tranzit's locations
// share one map: the start location tells Town, Farm and the Bus Depot apart, and the classic game type is the whole
// Tranzit. Each is saved in the settings file as profile_<key> (on or off), profile_<key>_preset and
// profile_<key>_gfx_<option>; a profile turned on for the first time starts as a copy of the global settings.
// TOML selects the global startup preset. Benchmarks and explicit environment/command-line presets use GLOBAL
// until a profile is enabled interactively.
struct Location {
  const char* key;
  const char* map;
  const char* gametype;  // nullptr: any (these are checked after the ones with a game type)
  const char* start;     // ui_zm_mapstartlocation; nullptr: any
  const char* label;     // the menu's (menus.lua, ProfileNames, in this order) and the log's
};
constexpr Location kLocations[] = {
    {"town", "zm_transit", nullptr, "town", "TOWN"},
    {"farm", "zm_transit", nullptr, "farm", "FARM"},
    {"bus_depot", "zm_transit", nullptr, "transit", "BUS DEPOT"},
    {"tranzit", "zm_transit", "zclassic", nullptr, "TRANZIT"},
    {"diner", "zm_transit_dr", nullptr, nullptr, "DINER"},
    {"nuketown", "zm_nuked", nullptr, nullptr, "NUKETOWN"},
    {"die_rise", "zm_highrise", nullptr, nullptr, "DIE RISE"},
    {"mob_of_the_dead", "zm_prison", nullptr, nullptr, "MOB OF THE DEAD"},
    {"buried", "zm_buried", nullptr, nullptr, "BURIED"},
    {"origins", "zm_tomb", nullptr, nullptr, "ORIGINS"},
};
constexpr int kNumLocations = int(sizeof(kLocations) / sizeof(kLocations[0]));

struct Profile {
  bool enabled = false;
  bool own = false;  // has settings of its own (else turning it on copies the global ones)
  int preset = kDefault;
  std::array<int, kNumOptions> custom{};
};

std::mutex g_profiles_mutex;
std::array<Profile, kNumLocations> g_profiles;  // under g_profiles_mutex
std::atomic<int> g_location{-1};                // the location loading or loaded (bo2zm_profile_map); -1: none
std::atomic<int> g_selected{0};                 // the menus' MAP PROFILE: 0 global, 1 + location
std::atomic<bool> g_force_global{false};        // session baseline, released by an explicit profile choice
std::atomic<int> g_note_preset{kDefault};       // the preset in effect, for the corner (any thread)

bool ForceGlobalAtStartup(rex::cvar::Source source, bool benchmark) {
  return benchmark || source == rex::cvar::Source::kEnvironment || source == rex::cvar::Source::kCommandLine;
}

int FindLocation(std::string_view map, std::string_view gametype, std::string_view start) {
  for (int l = 0; l < kNumLocations; ++l) {
    if (map == kLocations[l].map && kLocations[l].gametype && gametype == kLocations[l].gametype) {
      return l;
    }
  }
  for (int l = 0; l < kNumLocations; ++l) {
    if (map == kLocations[l].map && !kLocations[l].gametype && (!kLocations[l].start || start == kLocations[l].start)) {
      return l;
    }
  }
  return -1;
}

const char* ProfileLabel(int profile) {
  return profile <= 0 ? "GLOBAL" : kLocations[profile - 1].label;
}

// The profile a loaded map uses: 1 + its location if that profile is on, else 0 (global).
int ActiveProfile() {
  if (!bo2zm::mode::IsZombies()) {
    return 0;
  }
  const int l = g_location.load(std::memory_order_relaxed);
  if (l < 0 || g_force_global.load(std::memory_order_relaxed)) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_profiles_mutex);
  return g_profiles[l].enabled ? l + 1 : 0;
}

// The profile the Graphics menu shows and changes: in a match the one in use; in the menus the one MAP PROFILE picks
// when it is on, else the global one (which that map would use).
int EditedProfile() {
  if (!bo2zm::mode::IsZombies()) {
    return 0;
  }
  if (g_in_match.load(std::memory_order_relaxed)) {
    return ActiveProfile();
  }
  const int selected = g_selected.load(std::memory_order_relaxed);
  if (selected <= 0 || g_force_global.load(std::memory_order_relaxed)) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_profiles_mutex);
  return g_profiles[selected - 1].enabled ? selected : 0;
}

int PresetOf(int profile) {
  if (profile <= 0) {
    return g_index.load(std::memory_order_relaxed);
  }
  std::lock_guard<std::mutex> lock(g_profiles_mutex);
  return g_profiles[profile - 1].preset;
}

// An option's level in a profile, for its preset.
int LevelIn(int profile, int option) {
  if (profile <= 0) {
    return LevelOf(g_index.load(std::memory_order_relaxed), option);
  }
  std::lock_guard<std::mutex> lock(g_profiles_mutex);
  const Profile& p = g_profiles[profile - 1];
  return p.preset == kCustom ? p.custom[option] : kPresetLevels[p.preset][option];
}

// Under g_profiles_mutex.
void SaveProfile(int location) {
  const Profile& p = g_profiles[location];
  const std::string key = std::string("profile_") + kLocations[location].key;
  std::vector<std::pair<std::string, std::string>> values{{key, p.enabled ? "on" : "off"}};
  if (p.own) {
    values.emplace_back(key + "_preset", kNames[p.preset]);
    for (int o = 0; o < kNumOptions; ++o) {
      values.emplace_back(key + "_gfx_" + kOptions[o].name, kOptions[o].level_names[p.custom[o]]);
    }
  }
  bo2zm::settings::SetMany(values);
}

void LoadProfiles() {
  std::string on;
  std::lock_guard<std::mutex> lock(g_profiles_mutex);
  for (int l = 0; l < kNumLocations; ++l) {
    Profile& p = g_profiles[l];
    const std::string key = std::string("profile_") + kLocations[l].key;
    p.enabled = bo2zm::settings::Get(key) == std::optional<std::string>("on");
    if (const auto preset = bo2zm::settings::Get(key + "_preset")) {
      p.own = true;
      p.preset = IndexOf(*preset);
    }
    for (int o = 0; o < kNumOptions; ++o) {
      p.custom[o] = kPresetLevels[kDefault][o];
      if (const auto value = bo2zm::settings::Get(key + "_gfx_" + kOptions[o].name)) {
        for (int level = 0; level < kOptions[o].levels; ++level) {
          if (*value == kOptions[o].level_names[level]) {
            p.custom[o] = level;
          }
        }
      }
    }
    if (p.enabled) {
      on += std::string(on.empty() ? "" : ", ") + kLocations[l].label + " (" + kNames[p.preset] + ")";
    }
  }
  REXLOG_INFO("[preset] map profiles on: {}{}", on.empty() ? "none" : on,
              g_force_global.load(std::memory_order_relaxed)
                  ? " (GLOBAL override; enable a profile in Graphics to release)" : "");
}

// The preset saved in the settings file (bo2zm_settings.cpp): "preset = <name>".
std::optional<int> LoadSaved() {
  if (const auto value = bo2zm::settings::Get("preset")) {
    for (int i = 0; i < kPresets; ++i) {
      if (*value == kNames[i]) {
        return i;
      }
    }
  }
  return std::nullopt;
}

void Save(int index) {
  if (bo2zm::settings::Set("preset", kNames[index])) {
    g_saved = index;
  }
}

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

float ReadReal(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Read32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

void Write32(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + address, &v, 4);
}

// Split screen: the local views the game draws (1-4), from splitscreen_playerCount (an int, its value at +0x18). The
// game sets it to the local players when a match starts; the "splitscreen" bool stays 0 in a four-player local
// Zombies match (measured on Town, 2026-11-10), so it is not read. Read on the main thread every frame (EveryFrame,
// in a match only); a change re-applies the preset.
std::atomic<int> g_views{1};

int ReadViews(const uint8_t* base) {
  static std::optional<bo2zm::dvars::Dvar> count;
  if (!count) {
    count = bo2zm::dvars::Find("splitscreen_playerCount");
  }
  if (!count) {
    return 1;
  }
  return std::clamp<int>(int32_t(Read32(base, count->pointer + 0x18)), 1, 4);
}

// The split-screen cap (a preset index), or -1 with one view or bo2zm_splitscreen_preset = "same".
int SplitCap() {
  if (g_views.load(std::memory_order_relaxed) <= 1) {
    return -1;
  }
  const std::string& name = REXCVAR_GET(bo2zm_splitscreen_preset);
  return name == "same" ? -1 : std::min(IndexOf(name), kHighest);
}

// The model detail factor's multiplier for the views: bo2zm_splitscreen_model_scale with 3-4, its root with 2.
float SplitModelScale() {
  const int views = g_views.load(std::memory_order_relaxed);
  const double scale = REXCVAR_GET(bo2zm_splitscreen_model_scale);
  return views >= 3 ? float(scale) : views == 2 ? float(std::sqrt(scale)) : 1.0f;
}

Value ReadValue(const uint8_t* base, const DvarRecord& r) {
  Value v;
  v.type = r.type;
  if (r.type == kBool) {
    v.whole = base[r.pointer + kDvarCurrent] != 0;
  } else if (r.type == kReal) {
    const uint32_t bits = Read32(base, r.pointer + kDvarCurrent);
    std::memcpy(&v.real, &bits, 4);
  } else {
    v.whole = int32_t(Read32(base, r.pointer + kDvarCurrent));
  }
  return v;
}

Value ValueOf(const char* text, int type) {
  Value v;
  v.type = type;
  if (type == kReal) {
    v.real = std::strtof(text, nullptr);
  } else {
    v.whole = int32_t(std::strtol(text, nullptr, 10));
  }
  return v;
}

// 6-bit codes of the output pass font (bo2zm_output_gamma_ramp.frag): 0 end, 1 space, 2-11 digits, 12-37 A-Z,
// 38 ':', 39 '.', 40 '-', 41 '%', 42 '/'.
uint32_t Code(char c) {
  if (c >= '0' && c <= '9') return 2 + uint32_t(c - '0');
  if (c >= 'A' && c <= 'Z') return 12 + uint32_t(c - 'A');
  if (c >= 'a' && c <= 'z') return 12 + uint32_t(c - 'a');
  switch (c) {
    case ':': return 38;
    case '.': return 39;
    case '-': return 40;
    case '%': return 41;
    case '/': return 42;
    default: return 1;
  }
}

// Up to 20 characters, 5 codes per word.
void Encode(std::string_view text, uint32_t (&words)[4]) {
  for (int i = 0; i < 4; ++i) {
    words[i] = 0;
  }
  for (size_t i = 0; i < text.size() && i < 20; ++i) {
    words[i / 5] |= Code(text[i]) << (6 * (i % 5));
  }
}

void ShowNote(std::string_view text, double seconds) {
  uint32_t words[4];
  Encode(text, words);
  for (int i = 0; i < 4; ++i) {
    g_note[i].store(words[i], std::memory_order_relaxed);
  }
  g_note_until.store(Ns(Clock::now()) + int64_t(seconds * 1e9), std::memory_order_release);
}

// Runs commands with the cheat flag cleared on the dvars the table manages, then puts the flags back.
void ExecuteNoTraps(PPCContext& ctx, uint8_t* base, const std::string& commands) {
  std::array<uint32_t, kNumSettings> flags{};
  for (size_t i = 0; i < kNumSettings; ++i) {
    const uint32_t p = g_managed[i].reg_index.pointer;
    if (p) {
      flags[i] = Read32(base, p + kDvarFlags);
      Write32(base, p + kDvarFlags, flags[i] & ~kFlagTrap);
    }
  }
  bo2zm::autoexec::ExecuteCommands(ctx, base, commands);
  for (size_t i = 0; i < kNumSettings; ++i) {
    const uint32_t p = g_managed[i].reg_index.pointer;
    if (p) {
      // Keep whatever the commands changed, except the cheat bit, which goes back as it was.
      const uint32_t now = Read32(base, p + kDvarFlags);
      Write32(base, p + kDvarFlags, (now & ~kFlagTrap) | (flags[i] & kFlagTrap));
    }
  }
}

// Brings the dvars to the preset's values. A dvar whose value is not the one we wrote last belongs to the game
// (first time, or a script changed it): that value becomes its original, the one "high" puts back.
void Apply(PPCContext& ctx, uint8_t* base, const char* given_reason) {
  const int profile = ActiveProfile();
  const int index = PresetOf(profile);
  const int cap = FrontEndCap();
  std::string why = std::string(given_reason) + (profile ? std::string(", ") + ProfileLabel(profile) +
                                                            " profile" : std::string());
  if (cap >= 0) {
    why += std::string(", campaign front end: at most ") + kNames[cap] + ", far objects simple, every model kept";
  }
  const int split = SplitCap();
  const int views = g_views.load(std::memory_order_relaxed);
  if (views > 1) {
    why += fmt::format(", split screen with {} views: at most {}, model detail factor x{:.2f}", views,
                       split >= 0 ? kNames[split] : "the player's preset", SplitModelScale());
  }
  const char* const reason = why.c_str();
  std::string commands;
  std::string changes;
  std::vector<std::pair<size_t, Value>> goals;
  int levels[kNumOptions];
  std::string level_text;
  for (int o = 0; o < kNumOptions; ++o) {
    levels[o] = LevelIn(profile, o);
    if (cap >= 0 && o == kFar) {
      levels[o] = kFarSimple;
    } else if (cap >= 0) {
      levels[o] = std::min(levels[o], kPresetLevels[cap][o]);
    }
    if (split >= 0 && o != kFar) {
      levels[o] = std::min(levels[o], kPresetLevels[split][o]);
    }
    level_text += std::string(level_text.empty() ? "" : ", ") + kOptions[o].name + " " +
                  kOptions[o].level_names[levels[o]];
  }
  for (size_t i = 0; i < kNumSettings; ++i) {
    Managed& g = g_managed[i];
    const Setting& a = kSettings[i];
    if (!g.reg_index.pointer) {
      if (!g.warned) {
        g.warned = true;
        REXLOG_WARN("[preset] dvar {} is not registered: skipped", a.dvar);
      }
      continue;
    }
    const Value actual = ReadValue(base, g.reg_index);
    if (!g.written || !(actual == *g.written)) {
      g.original = actual;
    }
    const char* value = ModelDetailNative() && a.option == kModel ? nullptr : a.values[levels[a.option]];
    // FAR OBJECTS SIMPLE: the far plane goes out (see kFarSimple); the draw distance stays for the static models.
    if (a.option == kDistance && levels[kFar] == kFarSimple) {
      value = nullptr;
    }
    const Value goal = value ? ValueOf(value, g.reg_index.type) : *g.original;
    g.written = value ? std::optional<Value>(goal) : std::nullopt;
    goals.emplace_back(i, goal);
    if (!(goal == actual)) {
      commands += "set " + std::string(a.dvar) + " " + goal.Text() + ";";
      changes += (changes.empty() ? "" : ", ") + std::string(a.dvar) + " " + actual.Text() + " -> " +
                 goal.Text();
    }
  }
  if (!commands.empty()) {
    ExecuteNoTraps(ctx, base, commands);
  }
  // Read back: a dvar that did not take is logged, with what it holds now.
  std::string misses;
  for (const auto& [i, goal] : goals) {
    const Value now = ReadValue(base, g_managed[i].reg_index);
    if (!(now == goal)) {
      misses += (misses.empty() ? "" : ", ") + std::string(kSettings[i].dvar) + " is " + now.Text() +
                " (wanted " + goal.Text() + ")";
      g_managed[i].written.reset();
    }
  }
  for (const auto& [i, goal] : goals) {
    for (const char* forced : kForced) {
      if (std::strcmp(kSettings[i].dvar, forced) == 0) {
        g_forced[i].store(g_managed[i].written ? 1 + goal.whole : 0, std::memory_order_relaxed);
      }
    }
  }
  g_mips_minus.store(kMipsMinus[levels[kTexture]], std::memory_order_relaxed);
  g_model_factor.store(ModelDetailNative() ? kModelFactors[levels[kModel]] * SplitModelScale() : 1.0f,
                       std::memory_order_relaxed);
  // The draw distance: r_zfar's value for the level (0 = the map's own), whether or not FAR OBJECTS moved the dvar.
  float far = 0.0f;
  for (const Setting& s : kSettings) {
    if (std::strcmp(s.dvar, "r_zfar") == 0 && s.values[levels[kDistance]]) {
      far = std::strtof(s.values[levels[kDistance]], nullptr);
    }
  }
  g_far_plane.store(far, std::memory_order_relaxed);
  g_far_simple.store(far > 0.0f && levels[kFar] == kFarSimple, std::memory_order_relaxed);
  // In the campaign's front end no model past the draw distance is dropped for being small.
  g_far_simple_size.store(cap >= 0 ? 0.0f : float(REXCVAR_GET(bo2zm_far_simple_size)), std::memory_order_relaxed);
  if (far > 0.0f) {
    REXLOG_INFO("[preset] far objects {}: draw distance {:.0f}, the map's own far plane {:.0f}",
                kOptions[kFar].level_names[levels[kFar]], far, ReadReal(base, kMapFarPlane));
  }
  REXLOG_INFO("[preset] {} ({}; {}): {}; textures {} mip levels lower", kNames[index], reason, level_text,
              changes.empty() ? std::string("no dvar to change") : changes, kMipsMinus[levels[kTexture]]);
  if (!misses.empty()) {
    REXLOG_WARN("[preset] {}: did not take: {}", kNames[index], misses);
  }
}

// True if a dvar we set no longer holds our value (a level script or the game changed it).
bool Undone(const uint8_t* base, std::string& which) {
  for (size_t i = 0; i < kNumSettings; ++i) {
    const Managed& g = g_managed[i];
    if (g.reg_index.pointer && g.written) {
      const Value now = ReadValue(base, g.reg_index);
      if (!(now == *g.written)) {
        which += (which.empty() ? "" : ", ") + std::string(kSettings[i].dvar) + " " + g.written->Text() + " -> " +
                  now.Text();
      }
    }
  }
  return !which.empty();
}

// bo2zm_test_console: "S:cmd;cmd|S:cmd".
struct ConsoleStep {
  double seconds;
  std::string commands;
};

std::vector<ConsoleStep> ReadStepsConsole() {
  std::vector<ConsoleStep> steps;
  const std::string text = REXCVAR_GET(bo2zm_test_console);
  size_t i = 0;
  while (i < text.size()) {
    size_t end = text.find('|', i);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string step = text.substr(i, end - i);
    i = end + 1;
    const size_t two_points = step.find(':');
    if (two_points == std::string::npos) {
      REXLOG_WARN("[preset] test console step \"{}\" has no 'S:': ignored", step);
      continue;
    }
    steps.push_back({std::strtod(step.c_str(), nullptr), step.substr(two_points + 1)});
  }
  return steps;
}

// Logs the value of every dvar named in "set NAME ..." commands.
void ReadBack(const uint8_t* base, const std::string& commands) {
  size_t i = 0;
  std::string report;
  while ((i = commands.find("set ", i)) != std::string::npos) {
    if (i > 0 && commands[i - 1] != ';' && commands[i - 1] != ' ') {
      i += 4;
      continue;
    }
    i += 4;
    const size_t end = commands.find_first_of(" ;", i);
    const std::string label = commands.substr(i, end == std::string::npos ? std::string::npos : end - i);
    DvarRecord r;
    if (const auto found = bo2zm::dvars::Find(label)) {
      r = DvarRecord{found->pointer, found->kind};
    }
    report += (report.empty() ? "" : ", ") + label + " = " +
               (r.pointer ? ReadValue(base, r).Text() : std::string("(not registered)"));
  }
  if (!report.empty()) {
    REXLOG_INFO("[preset] test console read back: {}", report);
  }
}

}  // namespace

namespace {
std::atomic<const uint8_t*> g_cl_ingame{nullptr};  // cl_ingame's current value (a bool), once registered

// bo2zm_front_end_test_s: in the campaign's front end, the player's preset and the front-end settings take turns,
// and each period's frame rate is logged. Main thread, once a frame.
void FrontEndFrame() {
  static bool was_on = false;
  static Clock::time_point since{};
  static Clock::time_point step_start{};
  static int step = -1;
  static uint32_t frames = 0;
  constexpr const char* kSteps[2] = {"the player's preset", "the front-end settings"};
  const bool on = g_front_end.load(std::memory_order_relaxed);
  const int every = REXCVAR_GET(bo2zm_front_end_test_s);
  const auto now = Clock::now();
  auto log_step = [&] {
    if (step >= 0) {
      const double seconds = std::chrono::duration<double>(now - step_start).count();
      REXLOG_INFO("[preset] front end test: {:.1f} s with {}: {} frames, {:.1f} FPS", seconds, kSteps[step], frames,
                  seconds > 0 ? frames / seconds : 0.0);
    }
  };
  if (on != was_on) {
    was_on = on;
    if (!on && every > 0) {
      log_step();
    }
    since = now;
    step = -1;
    g_front_end_step.store(-1, std::memory_order_relaxed);
  }
  if (!on || every <= 0) {
    return;
  }
  const int current = int(std::chrono::duration_cast<std::chrono::seconds>(now - since).count() / every % 2);
  if (current != step) {
    log_step();
    step = current;
    frames = 0;
    step_start = now;
    g_front_end_step.store(current, std::memory_order_relaxed);
    g_pending.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[preset] front end test: {} for the next {} s", kSteps[current], every);
  }
  ++frames;
}
}  // namespace

bool FrontEnd() {
  return g_front_end.load(std::memory_order_relaxed) && g_front_end_step.load(std::memory_order_relaxed) != 0;
}

namespace {
std::atomic<bool> g_front_end_scene_kept{false};
std::atomic<int64_t> g_input_blocked_since{0};  // steady-clock ns when the block began, 0 = not blocked
}  // namespace

void SetFrontEndSceneKept(bool kept) {
  g_front_end_scene_kept.store(kept, std::memory_order_relaxed);
}

bool FrontEndSceneKept() {
  return g_front_end_scene_kept.load(std::memory_order_relaxed);
}

void SetInputBlocked(bool blocked) {
  const int64_t now = Ns(Clock::now());
  g_input_blocked_since.store(blocked ? std::max<int64_t>(now, 1) : 0, std::memory_order_relaxed);
  REXLOG_INFO("[preset] controller input {} the game", blocked ? "kept from" : "given back to");
}

bool InputBlocked() {
  const int64_t since = g_input_blocked_since.load(std::memory_order_relaxed);
  if (!since) {
    return false;
  }
  if (Ns(Clock::now()) - since > int64_t(15) * 1000000000) {  // a safety net if the veil never lifts
    g_input_blocked_since.store(0, std::memory_order_relaxed);
    REXLOG_WARN("[preset] controller input given back after 15 s");
    return false;
  }
  return true;
}

int FrontEndFpsCap() {
  return REXCVAR_GET(bo2zm_front_end_fps_cap);
}

void SetFrontEnd(bool on) {
  if (g_front_end.exchange(on, std::memory_order_relaxed) != on) {
    REXLOG_INFO("[preset] campaign front end {}: {}", on ? "loaded" : "left",
                on ? "its 3D scene gets bo2zm_front_end_preset" : "the player's preset again");
    g_pending.store(true, std::memory_order_relaxed);
  }
}

bool ClientInGame() {
  const uint8_t* value = g_cl_ingame.load(std::memory_order_relaxed);
  return value && *value != 0;
}


void DvarRegistered(uint8_t* base, uint32_t name, uint32_t dvar, int kind) {
  if (!name || !dvar) {
    return;
  }
  const char* label = reinterpret_cast<const char*>(base + name);
  if (std::strcmp(label, "cl_ingame") == 0) {
    g_cl_ingame.store(base + dvar + kDvarCurrent, std::memory_order_relaxed);
  }
  for (size_t i = 0; i < kNumSettings; ++i) {
    if (std::strcmp(label, kSettings[i].dvar) == 0) {
      g_managed[i].reg_index = DvarRecord{dvar, kind};
      const uint32_t flags = Read32(base, dvar + kDvarFlags);
      if (flags & kFlagFile) {
        Write32(base, dvar + kDvarFlags, flags & ~kFlagFile);
      }
      static int times = 0;
      if (times++ < int(kNumSettings) * 2) {
        REXLOG_INFO("[preset] dvar {} at {:08X}: kind {}, type {}, flags {:X}{}, value {}", label, dvar, kind,
                    Read32(base, dvar + 0x10), flags, (flags & kFlagFile) ? " (no longer saved to the config)" : "",
                    ReadValue(base, g_managed[i].reg_index).Text());
      }
    }
  }
}

void EveryFrame(PPCContext& ctx, uint8_t* base) {
  FrontEndFrame();
  static bool watching = false;
  if (!watching) {
    watching = true;
    const int configured = IndexOf(rex::cvar::GetFlagByName("bo2zm_preset"));
    // Read before the saved preset is put in the cvar below, which changes where it says its value came from.
    const auto preset_source = rex::cvar::GetFlagSource("bo2zm_preset");
    const bool preset_given = preset_source != rex::cvar::Source::kDefault;
    if (!preset_given) {
      if (const auto saved = LoadSaved(); saved && *saved != configured) {
        rex::cvar::SetFlagByName("bo2zm_preset", kNames[*saved]);
        REXLOG_INFO("[preset] {} from {}", kNames[*saved], bo2zm::settings::File().string());
      }
    }
    g_saved = IndexOf(rex::cvar::GetFlagByName("bo2zm_preset"));
    g_index.store(g_saved, std::memory_order_relaxed);
    LoadCustom();
    g_force_global.store(ForceGlobalAtStartup(preset_source, rex::cvar::Query<bool>("bo2zm_benchmark")),
                         std::memory_order_relaxed);
    LoadProfiles();
    // The notification arrives with the registry lock held (SetFlagFromSource): only atomics are touched.
    rex::cvar::RegisterChangeCallback("bo2zm_preset", [](std::string_view, std::string_view value) {
      g_index.store(IndexOf(value), std::memory_order_relaxed);
      g_pending.store(true, std::memory_order_relaxed);
    });
    REXLOG_INFO("[preset] {} (applied when a map loads)", kNames[g_index.load()]);
  }

  // The menu's GRAPHICS PRESET option reads the game dvar bo2zm_preset_ui; a change from any side (menu, button
  // combination, settings overlay) is written to the settings file.
  static int shown = -1;
  static auto shown_levels = [] {
    std::array<int, kNumOptions> levels;
    levels.fill(-1);
    return levels;
  }();
  if (const int index = g_index.load(std::memory_order_relaxed); index != g_saved) {
    Save(index);
  }
  // The menu shows the profile it edits (EditedProfile): the global one, or a map's.
  const int edited = EditedProfile();
  if (const int index = PresetOf(edited); index != shown) {
    shown = index;
    bo2zm::autoexec::ExecuteCommands(ctx, base, "set bo2zm_preset_ui " + std::to_string(index));
  }
  // Each option's level in effect, for its selector (bo2zm_gfx_<option>_ui).
  {
    std::string commands;
    for (int o = 0; o < kNumOptions; ++o) {
      const int level = LevelIn(edited, o);
      if (level != shown_levels[o]) {
        shown_levels[o] = level;
        commands += std::string(commands.empty() ? "" : "; ") + "set bo2zm_gfx_" + kOptions[o].name + "_ui " +
                    std::to_string(level);
      }
    }
    if (!commands.empty()) {
      bo2zm::autoexec::ExecuteCommands(ctx, base, commands);
    }
    if (g_custom_dirty.exchange(false, std::memory_order_relaxed)) {
      SaveCustom();
    }
  }
  // MAP PROFILE (in a match: the profile in use) and ENABLE PROFILE (2 = N/A: GLOBAL, or no map known).
  {
    static int shown_profile = -1;
    static int shown_on = -1;
    const bool in_match = g_in_match.load(std::memory_order_relaxed);
    const bool zombies = bo2zm::mode::IsZombies();
    const int profile = !zombies ? 0 : in_match ? ActiveProfile() : g_selected.load(std::memory_order_relaxed);
    int on = 2;
    if (const int l = in_match ? g_location.load(std::memory_order_relaxed)
                               : g_selected.load(std::memory_order_relaxed) - 1;
        zombies && l >= 0) {
      std::lock_guard<std::mutex> lock(g_profiles_mutex);
      on = !g_force_global.load(std::memory_order_relaxed) && g_profiles[l].enabled ? 1 : 0;
    }
    if (profile != shown_profile || on != shown_on) {
      shown_profile = profile;
      shown_on = on;
      bo2zm::autoexec::ExecuteCommands(ctx, base, "set bo2zm_profile_ui " + std::to_string(profile) +
                                                      "; set bo2zm_profile_on_ui " + std::to_string(on));
    }
    // The preset in effect (the corner shows it), and a change of profile in a match applies at once.
    static int last_active = 0;
    const int active = ActiveProfile();
    g_note_preset.store(PresetOf(active), std::memory_order_relaxed);
    if (active != last_active) {
      last_active = active;
      g_pending.store(true, std::memory_order_relaxed);
    }
  }
  // Model detail: how many models the version choice kept that the factor alone would have dropped, and how far.
  {
    static Clock::time_point last_kept = Clock::now();
    if (const auto now = Clock::now(); now - last_kept >= std::chrono::seconds(10)) {
      last_kept = now;
      uint32_t kept[4];
      for (int i = 0; i < 4; ++i) {
        kept[i] = g_kept[i].exchange(0, std::memory_order_relaxed);
      }
      const uint32_t dropped = g_dropped.exchange(0, std::memory_order_relaxed);
      const uint32_t far_skipped = g_far_skipped.exchange(0, std::memory_order_relaxed);
      const uint32_t held_back = g_held_back.exchange(0, std::memory_order_relaxed);
      if (held_back) {
        REXLOG_INFO("[preset] model detail, last 10 s: {} times a model got its second simplest version instead of "
                    "its simplest, which the game would not use yet", held_back);
      }
      if (far_skipped) {
        REXLOG_INFO("[preset] far skip, last 10 s: {} static models not drawn, wholly past the far plane ({:.0f})",
                    far_skipped, g_far_plane.load(std::memory_order_relaxed));
      }
      const uint32_t far_kept = g_far_kept.exchange(0, std::memory_order_relaxed);
      const uint32_t far_dropped = g_far_dropped.exchange(0, std::memory_order_relaxed);
      if (far_kept + far_dropped) {
        REXLOG_INFO("[preset] far objects simple, last 10 s: static models wholly past the draw distance ({:.0f}): "
                    "{} drawn in their simplest version, {} dropped as small",
                    g_far_plane.load(std::memory_order_relaxed), far_kept, far_dropped);
      }
      if (kept[0] + kept[1] + kept[2] + kept[3] + dropped) {
        REXLOG_INFO("[preset] model detail, last 10 s: {} models drawn that the factor alone would drop (by "
                    "distance: under 600 {}, 600-1200 {}, 1200-2500 {}, beyond {}), {} of those dropped as wholly past "
                    "the far plane ({:.0f})",
                    kept[0] + kept[1] + kept[2] + kept[3], kept[0], kept[1], kept[2], kept[3], dropped,
                    g_far_plane.load(std::memory_order_relaxed));
      }
    }
  }

  static Clock::time_point arrival{};
  static Clock::time_point checked{};
  static bool applied = false;
  static size_t next_step = 0;
  static int step_test = 0;
  static const std::vector<ConsoleStep> steps = ReadStepsConsole();

  // The button combination (OnPad, from bo2zm_pad.cpp): one step per press, wrapping around, in
  // menus too (a match loads with the new one).
  if (const int request = g_request.exchange(0, std::memory_order_relaxed); request != 0) {
    const int profile = ActiveProfile();  // a map's profile while it is in use, else the global one
    const int from = PresetOf(profile);
    const int new_value = from == kCustom ? (request > 0 ? 0 : kHighest)
                                          : ((from + request) % kStepped + kStepped) % kStepped;
    REXLOG_INFO("[preset] button combination: {} ({} profile)", kNames[new_value], ProfileLabel(profile));
    if (profile == 0) {
      rex::cvar::SetFlagByName("bo2zm_preset", kNames[new_value]);  // the callback above marks it pending
      g_index.store(new_value, std::memory_order_relaxed);
    } else {
      std::lock_guard<std::mutex> lock(g_profiles_mutex);
      g_profiles[profile - 1].preset = new_value;
      g_profiles[profile - 1].own = true;
      SaveProfile(profile - 1);
    }
    g_pending.store(true, std::memory_order_relaxed);
    ShowNote(kNotes[new_value], 2.5);  // the press's answer, in menus too
  }

  static bool client_was_in = false;
  if (ClientInGame() != client_was_in) {
    client_was_in = !client_was_in;
    REXLOG_INFO("[preset] the game's client is {} a match (cl_ingame)", client_was_in ? "in" : "out of");
  }
  // The server's first entity has its client (a match this console hosts), or the client is in a game (one joined).
  const bool present = Read32(base, kGEntities + kEntClient) >= 0x82000000u || ClientInGame();
  if (!present) {
    if (g_in_match.exchange(false, std::memory_order_relaxed)) {
      REXLOG_INFO("[preset] map unloaded: the preset is applied again on the next map");
      g_location.store(-1, std::memory_order_relaxed);
    }
    applied = false;
    return;
  }
  const auto now = Clock::now();
  if (!g_in_match.exchange(true, std::memory_order_relaxed)) {
    arrival = now;
  }
  const double seconds = std::chrono::duration<double>(now - arrival).count();


  // bo2zm_preset_test_s: one step lower every N seconds from 2 s after the map loads.
  if (const int every = REXCVAR_GET(bo2zm_preset_test_s); every > 0 && seconds >= 2.0) {
    const int step = 1 + int((seconds - 2.0) / every);
    if (step != step_test) {
      step_test = step;
      const int new_value = ((kHighest - (step - 1)) % kStepped + kStepped) % kStepped;
      REXLOG_INFO("[preset] test: step {} at {:.1f} s after the map loaded: {}", step, seconds, kNames[new_value]);
      rex::cvar::SetFlagByName("bo2zm_preset", kNames[new_value]);
      g_index.store(new_value, std::memory_order_relaxed);
      g_pending.store(true, std::memory_order_relaxed);
    }
  }

  // Two seconds after the map loads (its scripts set up their dvars first), then on every change.
  if (seconds < 2.0) {
    return;
  }
  if (const int views = ReadViews(base); views != g_views.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[preset] split screen: {} -> {} views", g_views.load(std::memory_order_relaxed), views);
    g_views.store(views, std::memory_order_relaxed);
    g_pending.store(true, std::memory_order_relaxed);
  }
  const int index = g_index.load(std::memory_order_relaxed);
  if (!applied || g_pending.exchange(false, std::memory_order_relaxed)) {
    Apply(ctx, base, applied ? "changed" : "map loaded");
    applied = true;
    checked = now;
  } else if (now - checked >= std::chrono::seconds(5)) {
    checked = now;
    std::string which;
    if (Undone(base, which)) {
      Apply(ctx, base, ("the game changed " + which).c_str());
    }
  }

  // bo2zm_test_console, after the preset so that its commands win.
  while (next_step < steps.size() && seconds >= steps[next_step].seconds) {
    const ConsoleStep& step = steps[next_step++];
    REXLOG_INFO("[preset] test console at {:.1f} s: {}", seconds, step.commands);
    // "png NAME" and "rdc" are handled here; the rest goes to the game's console.
    std::string console;
    size_t i = 0;
    while (i <= step.commands.size()) {
      size_t end = step.commands.find(';', i);
      if (end == std::string::npos) {
        end = step.commands.size();
      }
      std::string command = step.commands.substr(i, end - i);
      i = end + 1;
      command.erase(0, command.find_first_not_of(' '));
      if (command.rfind("png ", 0) == 0) {
        bo2zm::capture::RequestCapture(command.substr(4));
      } else if (command == "rdc") {
        bo2zm::autoexec::CaptureRenderDoc();
      } else if (command.rfind("preset ", 0) == 0) {
        rex::cvar::SetFlagByName("bo2zm_preset", command.substr(7));  // the change callback applies it
      } else if (!command.empty()) {
        console += command + ";";
      }
    }
    if (!console.empty()) {
      bo2zm::autoexec::ExecuteCommands(ctx, base, console);
      ReadBack(base, console);
    }
  }
}

void AfterForceShadows(uint8_t* base) {
  for (size_t i = 0; i < kNumSettings; ++i) {
    const int32_t value = g_forced[i].load(std::memory_order_relaxed);
    const uint32_t pointer = g_managed[i].reg_index.pointer;
    if (value > 0 && pointer) {
      base[pointer + kDvarCurrent] = uint8_t(value - 1);
    }
  }
}

void ShowBanner(std::string_view text, double delay_s, double seconds) {
  uint32_t words[8] = {};
  for (size_t i = 0; i < text.size() && i < 40; ++i) {
    words[i / 5] |= Code(text[i]) << (6 * (i % 5));
  }
  const int64_t from = Ns(Clock::now()) + int64_t(delay_s * 1e9);
  g_banner_until.store(0, std::memory_order_release);  // nothing half written shows
  for (int i = 0; i < 8; ++i) {
    g_banner[i].store(words[i], std::memory_order_relaxed);
  }
  g_banner_from.store(from, std::memory_order_relaxed);
  g_banner_until.store(from + int64_t(seconds * 1e9), std::memory_order_release);
}

namespace {
// A run: compiles less than 2 s apart. It shows from its 8th compile in a match (a new effect in normal play compiles
// one or two), and counts as going on until 3 s after its last.
constexpr int kVisitRun = 8;
std::mutex g_visit_mutex;
Clock::time_point g_visit_last{};
int g_visit_count = 0;
std::atomic<int64_t> g_visit_until{0};
}  // namespace

void NoteFirstVisitCompile() {
  const auto now = Clock::now();
  int count = 0;
  {
    std::lock_guard lock(g_visit_mutex);
    if (now - g_visit_last > std::chrono::seconds(2)) g_visit_count = 0;
    g_visit_last = now;
    count = ++g_visit_count;
  }
  if (count < kVisitRun || !bo2zm::cheats::InMatch()) return;
  if (count == kVisitRun) REXLOG_INFO("[preset] first visit: the map's graphics are being prepared");
  g_visit_until.store(Ns(now) + 3'000'000'000ll, std::memory_order_relaxed);
  ShowBanner("FIRST VISIT - PREPARING GRAPHICS " + std::to_string(count), 0.0, 3.0);
}

bool FirstVisitBuilding() { return Ns(Clock::now()) < g_visit_until.load(std::memory_order_relaxed); }

std::string LoadedMap() {
  std::lock_guard<std::mutex> lock(g_map_mutex);
  return g_map;
}

std::string LoadedLocation() {
  const int location = g_location.load(std::memory_order_relaxed);
  return location >= 0 ? kLocations[location].label : "";
}

std::string PresetLabel() {
  return kNotes[g_note_preset.load(std::memory_order_relaxed)] + 8;  // after "PRESET: "
}

bool OnScreenBanner(uint32_t (&codes)[8]) {
  // The failsafe switched the four cores off: once per time it does, for 3 seconds.
  static bool shown_for_this_trip = false;
  if (bo2zm::CoresSwitchedOff()) {
    if (!shown_for_this_trip) {
      shown_for_this_trip = true;
      constexpr std::string_view kText = "FAILSAFE: REVERTED TO 3 CORES";
      ShowBanner(kText, 0.0, 3.0);
      REXLOG_WARN("[preset] on screen for 3 s: {}", kText);
    }
  } else {
    shown_for_this_trip = false;
  }
  const int64_t until = g_banner_until.load(std::memory_order_acquire);
  const int64_t now = Ns(Clock::now());
  if (until == 0 || now >= until || now < g_banner_from.load(std::memory_order_relaxed)) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    codes[i] = g_banner[i].load(std::memory_order_relaxed);
  }
  return true;
}

bool OnScreenNote(uint32_t (&codes)[4]) {
  const int64_t until = g_note_until.load(std::memory_order_acquire);
  if (until == 0 || Ns(Clock::now()) >= until) {
    if (!REXCVAR_GET(bo2zm_preset_on_screen)) {
      return false;
    }
    // The preset in effect (in a menu: the one the next match loads with). On the Switch the label names the CPU
    // cores the game runs on right now instead of "PRESET:" ("VERY LOW - 3 CORES" / "VERY LOW - 4 CORES"), so a player
    // always sees which way the game runs: 3 with a normal launch, with CPU CORES at 3 or after the failsafe, 4 only
    // with a 4-core launch and CPU CORES at 4. The PC (CoresInUse 0) keeps "PRESET:".
    const int preset = g_note_preset.load(std::memory_order_relaxed);
    if (const int cores = bo2zm::CoresInUse(); cores == 3 || cores == 4) {
      static const auto labels = [] {
        std::array<std::array<std::array<uint32_t, 4>, kPresets>, 2> encoded{};
        for (int n = 0; n < 2; ++n) {
          for (int i = 0; i < kPresets; ++i) {
            uint32_t words[4];
            Encode(std::string(kNotes[i] + 8) + (n ? " - 4 CORES" : " - 3 CORES"), words);  // after "PRESET: "
            std::copy(std::begin(words), std::end(words), encoded[n][i].begin());
          }
        }
        return encoded;
      }();
      const auto& label = labels[cores - 3][preset];
      std::copy(label.begin(), label.end(), std::begin(codes));
      return true;
    }
    Encode(kNotes[preset], codes);
    return true;
  }
  for (int i = 0; i < 4; ++i) {
    codes[i] = g_note[i].load(std::memory_order_relaxed);
  }
  return true;
}

uint16_t OnPad(const bo2zm::pad::Pad& pad) {
  if (!(pad.buttons & bo2zm::pad::kBack)) {
    return 0;
  }
  if (pad.Pressed(bo2zm::pad::kUp)) {
    g_request.fetch_add(1, std::memory_order_relaxed);
  }
  if (pad.Pressed(bo2zm::pad::kDown)) {
    g_request.fetch_sub(1, std::memory_order_relaxed);
  }
  return bo2zm::pad::kUp | bo2zm::pad::kDown;
}

uint32_t MipsMinus() {
  return g_mips_minus.load(std::memory_order_relaxed);
}

void SetFromMenu(std::string_view value) {
  int index = -1;
  if (value.size() == 1 && value[0] >= '0' && value[0] < '0' + kPresets) {
    index = value[0] - '0';
  } else {
    for (int i = 0; i < kPresets; ++i) {
      if (value == kNames[i]) {
        index = i;
      }
    }
  }
  if (index < 0) {
    REXLOG_WARN("[preset] unknown preset from the menu: {}", value);
    return;
  }
  if (const int profile = EditedProfile(); profile != 0) {
    {
      std::lock_guard<std::mutex> lock(g_profiles_mutex);
      Profile& p = g_profiles[profile - 1];
      if (p.preset == index) {
        return;
      }
      p.preset = index;
      p.own = true;
      SaveProfile(profile - 1);
    }
    REXLOG_INFO("[preset] menu: {} ({} profile)", kNames[index], ProfileLabel(profile));
    g_pending.store(true, std::memory_order_relaxed);
    return;
  }
  if (index == g_index.load(std::memory_order_relaxed)) {
    return;
  }
  REXLOG_INFO("[preset] menu: {}", kNames[index]);
  rex::cvar::SetFlagByName("bo2zm_preset", kNames[index]);  // the change callback marks it pending
}

bool SetOptionFromMenu(std::string_view name, std::string_view value) {
  int option = -1;
  for (int o = 0; o < kNumOptions; ++o) {
    if (name == kOptions[o].name) {
      option = o;
    }
  }
  if (option < 0) {
    return false;
  }
  int level = -1;
  for (int l = 0; l < kOptions[option].levels; ++l) {
    if (value == kOptions[option].level_names[l] || (value.size() == 1 && value[0] == '0' + l)) {
      level = l;
    }
  }
  if (level < 0) {
    REXLOG_WARN("[preset] gfx_{}: unknown level {}", kOptions[option].name, value);
    return true;
  }
  if (const int profile = EditedProfile(); profile != 0) {
    {
      std::lock_guard<std::mutex> lock(g_profiles_mutex);
      Profile& p = g_profiles[profile - 1];
      const int current = p.preset == kCustom ? p.custom[option] : kPresetLevels[p.preset][option];
      if (level == current) {
        return true;
      }
      if (p.preset != kCustom) {  // custom starts from the preset in use
        for (int o = 0; o < kNumOptions; ++o) {
          p.custom[o] = kPresetLevels[p.preset][o];
        }
      }
      p.custom[option] = level;
      p.preset = kCustom;
      p.own = true;
      SaveProfile(profile - 1);
    }
    REXLOG_INFO("[preset] menu: {} {} (custom, {} profile)", kOptions[option].name, kOptions[option].level_names[level],
                ProfileLabel(profile));
    g_pending.store(true, std::memory_order_relaxed);
    return true;
  }
  const int index = g_index.load(std::memory_order_relaxed);
  if (level == LevelOf(index, option)) {
    return true;
  }
  // Custom starts from the preset in use.
  if (index != kCustom) {
    for (int o = 0; o < kNumOptions; ++o) {
      g_custom[o].store(kPresetLevels[index][o], std::memory_order_relaxed);
    }
  }
  g_custom[option].store(level, std::memory_order_relaxed);
  g_custom_dirty.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[preset] menu: {} {} (custom)", kOptions[option].name, kOptions[option].level_names[level]);
  if (index != kCustom) {
    rex::cvar::SetFlagByName("bo2zm_preset", kNames[kCustom]);  // the change callback marks it pending
  } else {
    g_pending.store(true, std::memory_order_relaxed);
  }
  return true;
}

void MapLoading(std::string_view value) {
  std::string_view words[3];
  for (auto& word : words) {
    while (!value.empty() && value.front() == ' ') {
      value.remove_prefix(1);
    }
    const size_t end = std::min(value.find(' '), value.size());
    word = value.substr(0, end);
    value.remove_prefix(end);
  }
  const int location = bo2zm::mode::IsZombies() ? FindLocation(words[0], words[1], words[2]) : -1;
  g_location.store(location, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_map = std::string(words[0]) + " " + std::string(words[1]) + " " + std::string(words[2]);
  }
  const int profile = ActiveProfile();
  REXLOG_INFO("[preset] loading {} ({} {} {}): {} profile", location >= 0 ? kLocations[location].label : "a map with no "
              "profile", words[0], words[1], words[2], ProfileLabel(profile));
  g_pending.store(true, std::memory_order_relaxed);
}

void SelectProfileFromMenu(std::string_view value) {
  if (!bo2zm::mode::IsZombies()) {
    return;
  }
  if (g_in_match.load(std::memory_order_relaxed)) {
    return;  // in a match the selector only shows the profile in use
  }
  int profile = 0;
  for (const char c : value) {
    profile = c >= '0' && c <= '9' ? profile * 10 + (c - '0') : -1000;
  }
  if (profile >= 0 && profile <= kNumLocations) {
    g_selected.store(profile, std::memory_order_relaxed);
  }
}

void EnableProfileFromMenu(std::string_view value) {
  if (!bo2zm::mode::IsZombies()) {
    return;
  }
  const bool in_match = g_in_match.load(std::memory_order_relaxed);
  const int location = in_match ? g_location.load(std::memory_order_relaxed)
                                 : g_selected.load(std::memory_order_relaxed) - 1;
  if (location < 0) {
    return;  // GLOBAL, or a map with no profile: N/A
  }
  bool on = false;
  {
    std::lock_guard<std::mutex> lock(g_profiles_mutex);
    Profile& p = g_profiles[location];
    const bool forced = g_force_global.exchange(false, std::memory_order_relaxed);
    // 2 is N/A: the menu's left and right skip it (menus.lua, statusChoice); were it sent, it turns the profile the
    // other way.
    // While GLOBAL is forced the selector reports OFF, even if the saved profile is enabled.
    on = value == "1" ? true : value == "0" ? false : (forced || !p.enabled);
    // Releasing GLOBAL must reapply even when the saved enable flag already matches.
    if (on == p.enabled && !forced) {
      return;
    }
    if (on && !p.own) {  // the first time: a copy of the global settings
      p.own = true;
      p.preset = g_index.load(std::memory_order_relaxed);
      for (int o = 0; o < kNumOptions; ++o) {
        p.custom[o] = g_custom[o].load(std::memory_order_relaxed);
      }
    }
    p.enabled = on;
    SaveProfile(location);
  }
  REXLOG_INFO("[preset] {} profile {}{}", kLocations[location].label, on ? "on" : "off",
              in_match ? " (applied now)" : "");
  g_pending.store(true, std::memory_order_relaxed);
}

}  // namespace bo2zm::preset

// Dvar registration (see the top of the file): every dvar goes to the registry (bo2zm_dvars.cpp), then to the
// preset. The bool one is hooked in bo2zm_autoexec.cpp (sv_cheats), which does the same.
REX_EXTERN(__imp__sub_824985D0);
REX_HOOK_RAW(sub_824985D0) {
  const uint32_t label = ctx.r3.u32;
  __imp__sub_824985D0(ctx, base);
  bo2zm::dvars::Registered(base, label, ctx.r3.u32, bo2zm::dvars::kInt);
  bo2zm::preset::DvarRegistered(base, label, ctx.r3.u32, 1);
}
REX_EXTERN(__imp__sub_82498668);
REX_HOOK_RAW(sub_82498668) {
  const uint32_t label = ctx.r3.u32;
  __imp__sub_82498668(ctx, base);
  bo2zm::dvars::Registered(base, label, ctx.r3.u32, bo2zm::dvars::kFloat);
  bo2zm::preset::DvarRegistered(base, label, ctx.r3.u32, 2);
}
REX_EXTERN(__imp__sub_82498880);
REX_HOOK_RAW(sub_82498880) {
  const uint32_t label = ctx.r3.u32;
  __imp__sub_82498880(ctx, base);
  bo2zm::dvars::Registered(base, label, ctx.r3.u32, bo2zm::dvars::kEnum);
  bo2zm::preset::DvarRegistered(base, label, ctx.r3.u32, 3);
}

// sub_828A84F0, called every frame by the scene setup (sub_828AF5A0, at 0x828AF63C), copies sm_sunShadowSmall into
// sm_sunShadowSmallEnable and sets sm_sunEnable and sm_spotEnable back to 1 with Dvar_SetBool (sub_824991B0) when
// they are not (0x828A8548-0x828A858C): TU18 keeps both shadow kinds on. A preset that turns them off writes its
// value back into the dvar right after.
REX_EXTERN(__imp__sub_828A84F0);
REX_HOOK_RAW(sub_828A84F0) {
  __imp__sub_828A84F0(ctx, base);
  bo2zm::preset::AfterForceShadows(base);
}

// Model detail's version choice: see kModelFactors.
static float ReadFloat(const uint8_t* p) {
  uint32_t bits;
  std::memcpy(&bits, p, 4);
  bits = __builtin_bswap32(bits);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
// How far a model reaches from its origin, however it is turned: its radius (+168, sub_824DC938) or the farthest corner
// of its bounds (mins at +172, maxs at +184, sub_824DC940), whichever is more. The radius alone is around the bounds'
// center, which a big model (a cave wall piece) can have far from its origin.
static float Reach(const uint8_t* model) {
  float corner = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    const float most = std::max(std::fabs(ReadFloat(model + 172 + 4 * axis)), std::fabs(ReadFloat(model + 184 + 4 * axis)));
    corner += most * most;
  }
  return std::max(ReadFloat(model + 168), std::sqrt(corner));
}
REX_EXTERN(__imp__sub_824DE0F8);
// bo2zm_diag_model_detail: one model's version choice, when it changes (at most once a second per model name).
static void DiagModelDetail(const PPCContext& ctx, uint8_t* base, const uint8_t* model, int chosen) {
  static const std::string filter = REXCVAR_GET(bo2zm_diag_model_detail);
  if (filter.empty()) [[likely]] {
    return;
  }
  const uint32_t name_address =
      uint32_t(model[0]) << 24 | uint32_t(model[1]) << 16 | uint32_t(model[2]) << 8 | model[3];
  if (!name_address) {
    return;
  }
  const char* const name = reinterpret_cast<const char*>(base + name_address);
  if (!std::strstr(name, filter.c_str())) {
    return;
  }
  PPCContext own = ctx;  // the game's own pick, without the preset
  __imp__sub_824DE0F8(own, base);
  struct Last {
    int own = -2, chosen = -2;
    std::chrono::steady_clock::time_point at{};
  };
  static std::mutex mutex;
  static std::unordered_map<std::string, Last> last;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  Last& entry = last[name];
  if ((entry.own == own.r3.s32 && entry.chosen == chosen) || now - entry.at < std::chrono::seconds(1)) {
    return;
  }
  entry = {own.r3.s32, chosen, now};
  const int16_t versions = int16_t(uint16_t(model[196]) << 8 | model[197]);
  std::string limits;
  for (int i = 0; i < versions && i < 6; ++i) {
    limits += fmt::format(" {:.0f}", ReadFloat(model + 40 + 28 * i));
  }
  REXLOG_INFO("[preset] model detail {}: ramp distance {:.0f}, plain {:.0f}, reach {:.0f}, ramp {} (byte +7), "
              "versions{}; the game picks {}, the preset {} (factor {:.2f}, from {:08X})",
              name, ctx.f1.f64, ctx.f2.f64, Reach(model), model[7], limits, own.r3.s32, chosen,
              bo2zm::preset::g_model_factor.load(std::memory_order_relaxed), uint32_t(ctx.lr));
}
static void ChooseModelVersion(PPCContext& ctx, uint8_t* base) {
  const uint8_t* const model = base + ctx.r3.u32;
  static const bool far_skip = REXCVAR_GET(bo2zm_far_skip);
  if (const float far = bo2zm::preset::g_far_plane.load(std::memory_order_relaxed); far > 0.0f && far_skip) {
    const uint32_t from = uint32_t(ctx.lr);
    for (const uint32_t call : bo2zm::preset::kStaticModelCalls) {
      if (from == call) {
        const float plain = float(ctx.f2.f64);
        const float reach = Reach(model);
        if (plain - reach > far) {  // wholly past the draw distance
          // FAR OBJECTS SIMPLE (kFarSimple): the far plane is out, so a big model stays, in its simplest version
          // (unless the game itself puts it past its versions). The share is the same in world units: the plain
          // distance and the reach are both divided by the instance's scale.
          const float size = bo2zm::preset::g_far_simple_size.load(std::memory_order_relaxed);
          if (bo2zm::preset::g_far_simple.load(std::memory_order_relaxed) && reach >= size * plain) {
            const int16_t versions = int16_t(uint16_t(model[196]) << 8 | model[197]);
            __imp__sub_824DE0F8(ctx, base);
            if (ctx.r3.s32 >= 0 && versions > 0) {
              ctx.r3.s64 = versions - 1;
              bo2zm::preset::g_far_kept.fetch_add(1, std::memory_order_relaxed);
            }
            return;
          }
          ctx.r3.s64 = -1;  // HIDE: the GPU would clip all of it; SIMPLE: small on screen
          (bo2zm::preset::g_far_simple.load(std::memory_order_relaxed) ? bo2zm::preset::g_far_dropped
                                                                        : bo2zm::preset::g_far_skipped)
              .fetch_add(1, std::memory_order_relaxed);
          return;
        }
        break;
      }
    }
  }
  const float factor = bo2zm::preset::g_model_factor.load(std::memory_order_relaxed);
  if (factor == 1.0f) {
    __imp__sub_824DE0F8(ctx, base);
    return;
  }
  const int16_t versions = int16_t(uint16_t(model[196]) << 8 | model[197]);
  // Only the rigid (0) and skinned (1) ramps exist; anything else is left to the game.
  if (versions <= 0 || model[7] > 1) {
    __imp__sub_824DE0F8(ctx, base);
    return;
  }
  const auto limit = [model](int i) { return ReadFloat(model + 40 + 28 * i); };
  const auto first_within = [&](float distance) {
    int i = 0;
    while (i < versions && !(distance < limit(i))) {
      ++i;
    }
    return i;  // versions: past all of them
  };
  const float distance = float(ctx.f1.f64);
  const int game = first_within(distance);
  if (game == versions) {
    __imp__sub_824DE0F8(ctx, base);  // past its versions for the game too: the game's own answer
    return;
  }
  // bo2zm_model_detail_reach: the factor scales the distance to the model's nearest possible point, not to its origin.
  // A long model's origin can sit at one end: Nuketown's moving truck (versions to 900, 1,300 and 16,000 units) is
  // measured from its cab, so beside the back of the trailer it was 797 units away, x 4 = 3,188, which picked its
  // simplest version, a plain box, right in front of the player (within 214 units of the cab it kept its detail). Only
  // for long models (reaching at least kReachLong from their origin; the truck reaches 461): a small model's origin is
  // close to all of it anyway, and doing it for every model kept many small ones detailed, 0.8 ms more GPU a frame on
  // Town on a Switch Lite. Never better than the game's own pick.
  constexpr float kReachLong = 256.0f;
  static const bool from_reach = REXCVAR_GET(bo2zm_model_detail_reach);
  const float reach = from_reach ? Reach(model) : 0.0f;
  const float scaled = (reach >= kReachLong ? std::max(distance - reach, 0.0f) : distance) * factor;
  int chosen = std::max(first_within(scaled), game);
  if (chosen == versions) {  // the factor alone would drop it, the game draws it
    const float plain = float(ctx.f2.f64);
    const float size = bo2zm::preset::g_far_simple_size.load(std::memory_order_relaxed);
    if (const float far = bo2zm::preset::g_far_plane.load(std::memory_order_relaxed);
        far > 0.0f && plain - Reach(model) > far &&
        (!bo2zm::preset::g_far_simple.load(std::memory_order_relaxed) || Reach(model) < size * plain)) {
      ctx.r3.s64 = -1;  // wholly past the far plane (or, FAR OBJECTS SIMPLE, small past the draw distance)
      bo2zm::preset::g_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    int bucket = 0;
    while (bucket < 3 && plain >= bo2zm::preset::kKeptLimits[bucket]) {
      ++bucket;
    }
    bo2zm::preset::g_kept[bucket].fetch_add(1, std::memory_order_relaxed);
  }
  // A model the factor alone would drop (older builds simply hid it) gets its simplest version only where the game
  // itself uses it: some lose what the others have (on the console, Buried's far windows became solid blocks without
  // their glass), and where it vanished it showed what was behind it. Elsewhere at most the second simplest. Only for
  // these: holding back every model's simplest version cost 3.5% on Town at very low on a Switch Lite (52.7 FPS against
  // 54.6), LOW model detail's own saving.
  // And only when the simplest version has fewer parts (surfaces) than the one before it: its entry is the distance,
  // then that count (a 16-bit number at +4; 28 bytes an entry). Holding back every one of them cost 2.9% on Town at
  // very low on a Lite (53.2 FPS against 54.8): the second simplest versions have more parts, each a draw.
  const auto parts = [model](int i) {
    return int(uint16_t(model[40 + 28 * i + 4]) << 8 | model[40 + 28 * i + 5]);
  };
  if (chosen == versions && game < versions - 1 && parts(versions - 1) < parts(versions - 2)) {
    chosen = versions - 2;
    bo2zm::preset::g_held_back.fetch_add(1, std::memory_order_relaxed);
  }
  ctx.r3.s64 = std::min<int>(chosen, versions - 1);
}
REX_HOOK_RAW(sub_824DE0F8) {
  static const bool diag = !REXCVAR_GET(bo2zm_diag_model_detail).empty();
  if (diag) [[unlikely]] {
    const PPCContext before = ctx;
    ChooseModelVersion(ctx, base);
    DiagModelDetail(before, base, base + before.r3.u32, ctx.r3.s32);
    return;
  }
  ChooseModelVersion(ctx, base);
}
