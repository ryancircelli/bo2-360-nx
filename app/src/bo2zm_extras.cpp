// See bo2zm_extras.h.
//
// FPS counter: bo2zm_show_fps (drawn at the top left by the output pass, bo2zm_native_targets.cpp), saved as
// "fps_counter = on|off" in the settings file (bo2zm_settings.cpp). The fly combo and collision settings are saved
// too. The cheats themselves are not: they start off on each run (god mode starts as bo2zm_test_god, for test runs).
//
// FPS cap: PaceFrame runs at the start of the frame hook, right before sub_8240F058, the game's own frame timing (in
// the main loop sub_824120A8: it runs the event loop sub_8240CDE0 while it waits out com_maxfps, then measures the
// frame time; the server frame sub_82410C08, the client frame and the drawing follow). So the wait is where the game's
// own limit waits: the frame reads the pad after it (no added input delay) and its measured time starts there, so the
// game moves on by the same steady period each frame. It paces in a match only, so menus and the frames around
// loading run at the game's own rate. Each frame's end is scheduled from the last one's (not from when it ended), so
// short oversleeps even out instead of adding up; a frame later than one whole period starts the schedule again rather
// than running fast to catch up. On Windows the wait is a high resolution waitable timer: Sleep and the standard
// library's sleep wake on the system timer tick (15.6 ms unless something raised the timer resolution), too coarse for
// 22 to 67 ms frames. On the Switch the standard library's sleep is the kernel's, which is precise.
// The cap is 30 or 60: the screen refreshes at 60 Hz, so 15 and 45 could not be paced evenly (45 shows frames for one
// and two refreshes in turn). A 15 or 45 saved by an older build reads as 30 or 60.
//
// VSYNC (Switch): on, each image waits for the screen's refresh: one refresh at FPS CAP 60 (swap interval 1), two at 30
// (interval 2: the panel paces a steady 30, the smoothest choice when the game cannot hold 60). PaceFrame's timer does
// not pace then: two 30 Hz clocks, the process timer and the panel, beat against each other (most frames at 33.3 ms
// and one at 50 every few seconds, bo2zm_graphics_settings.cpp). Left to the panel alone, though, a game faster than
// the cap runs ahead until every image queue is full (the presentation queue, the renderer's frames in flight), and
// the pad is read that many frames before its image shows, so the controls feel late at 30. In a match PaceFrame
// therefore starts each frame on the screen's own refresh signal (vi's vsync event, bo2zm_switch.cpp), every second
// refresh at 30 and every one at 60: the game makes images exactly as fast as the panel shows them, nothing queues up,
// and there is no second clock to beat against. Each frame that is early starts a few ms after its refresh
// (bo2zm_vsync_late_start_ms) and reads the controls that much later. Without the signal (if vi does not provide it)
// the pacing is left to the panel alone. Off is the usual swap interval 0: the newest image is shown at the next
// refresh and the game never waits for it (Horizon does not tear either way). The interval is set on every image
// presented (switch_perf.cpp). The PC's presentation mode is chosen when its swapchain is created, so there VSYNC
// does nothing.
//
// DEBUG page settings (bo2zm_debug_options.cpp) are kept here with the rest, and the program shows the build and this
// run's log file to the page (bo2zm_build_ui, bo2zm_log_ui).
//
// CHEATS (saved, off by default): the cheat settings and the menu's cheat commands work only while it is on
// (bo2zm::cheats::MenuCheats). Off pauses them: SetCheats keeps each pick, the menu still shows it, and on gives it
// back. The test helpers below set the cheats directly, whatever CHEATS is.
//
// LOADING SCREEN VIDEO (saved, on by default): bo2zm_loading_video. Off, the loading screen of the four DLC
// maps shows the map's image, as in an online match, instead of playing the map's video (src/ui/menus.lua,
// WrapLoading). A value set in bo2zm.toml wins over the saved pick, so a test toml can turn the video off.

#include "bo2zm_extras.h"
#include "bo2zm_script_manager.h"
#include "bo2zm_launch_options.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>

REXCVAR_DECLARE(std::string, bo2zm_splitscreen_preset);
REXCVAR_DECLARE(double, bo2zm_splitscreen_model_scale);
REXCVAR_DECLARE(int32_t, bo2zm_ipak_mb_s_in_match);
#if REX_PLATFORM_WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#include "bo2zm_autoexec.h"
#include "bo2zm_cheats.h"
#include "bo2zm_debug_options.h"
#include "bo2zm_mode.h"
#include "bo2zm_preset.h"
#include "bo2zm_relaunch.h"
#include "bo2zm_settings.h"
#include "bo2zm_switch.h"

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfSwapIntervalForce(int vblanks);
extern "C" uint32_t RexSwitchPerfTakeDequeueWaitUs(void);
#endif

REXCVAR_DECLARE(bool, bo2zm_show_fps);
REXCVAR_DECLARE(bool, fullscreen);
REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse);
REXCVAR_DEFINE_BOOL(bo2zm_vsync_drain, true, "Graphics",
                    "VSYNC: when the presenter had to wait for a free buffer (two finished frames already queued for "
                    "the screen), the next frame starts one refresh later, so finished frames stop waiting in the "
                    "queue (input delay). false = as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_vsync_late_start_ms, 8, "Graphics",
                     "VSYNC: a frame that is ready early starts up to this many ms after its refresh instead of on it, "
                     "so the game reads the controls later and the frame is still shown on the same refresh (less input "
                     "delay). The delay grows by 1 ms every 60 frames in time and halves at each frame that comes after "
                     "its refresh. 0 = on the refresh")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, bo2zm_preset_on_screen);
REXCVAR_DEFINE_STRING(bo2zm_test_start, "", "BO2",
                      "Test helper: the first game lobby that opens starts this match by itself, \"<map> <start "
                      "location> <game type> <mode group>\" (Town: \"zm_transit town zstandard zsurvival\"), and the "
                      "home menu opens the local lobby by itself. Multiplayer: map and game type, e.g. "
                      "mp_hijacked dm. Empty = "
                      "off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_test_start_wait_s, 0, "BO2",
                     "Test helper, with bo2zm_test_start: seconds the local lobby waits before it starts the match, "
                     "so test pad players 2-4 (bo2zm_test_buttons_p2..p4) can press Start to join split screen. "
                     "0 = 1.5 s")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_test_lobby_only, false, "BO2",
                    "Test helper, with bo2zm_test_start: select the match and stay in the local lobby")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_test_god, false, "BO2",
                    "God mode at start-up (keep player 0 invulnerable). The Extras menu turns it on or off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_test_ignore, false, "BO2",
                    "Test helper: zombies ignore player 0 from start-up. The Extras menu turns it on or off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_test_ammo, false, "BO2",
                    "Test helper: infinite ammo at start-up. The Extras menu turns it on or off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_loading_video, true, "BO2",
                    "The loading screen of Die Rise, Mob of the Dead, Buried and Origins plays the map's video (the "
                    "game's own). false = the map's loading image, as in an online match. Extras > LOADING SCREEN "
                    "VIDEO sets it too and saves the pick (loading_video); a value set here wins over the saved pick");

namespace {

using bo2zm::cheats::kFlyOff;
using bo2zm::cheats::kNoclip;
using bo2zm::cheats::kUfo;

bool LocalCamoAssetsReady() {
#if defined(BO2_GAME_CAMPAIGN)
  return false;
#else
  const auto* runtime = rex::Runtime::instance();
  if (!runtime || bo2zm::mode::IsZombies()) {
    return false;
  }
  // The supported TU supplies the personalization materials and image payloads.
  // Keep the Local menu closed to these options when its installation is incomplete.
  const auto present = [](const std::filesystem::path& root, const char* name) {
    if (root.empty()) return false;
    std::error_code error;
    const auto path = root / name;
    return std::filesystem::is_regular_file(path, error) && !error &&
           std::filesystem::file_size(path, error) > 0 && !error;
  };
  const auto& game = runtime->game_data_root();
  const auto& update = runtime->update_data_root();
  return present(game, "common_mp.ff") && present(game, "base.ipak") &&
         present(update, "common_patch_mp.ff") && present(update, "patch_mp.ff") &&
         present(update, "patch_ui_mp.ff") && present(update, "patch_all.ipak");
#endif
}

int FpsCounter() {
  return REXCVAR_GET(bo2zm_show_fps);
}
void SetFpsCounter(int on) {
  rex::cvar::SetFlagByName("bo2zm_show_fps", on ? "true" : "false");
}
int LoadingVideo() {
  return REXCVAR_GET(bo2zm_loading_video);
}
constexpr const char* kSplitQuality[] = {"same", "lowest", "very_low", "low", "medium", "high"};
constexpr const char* kIpakRates[] = {"4", "16", "0"};
constexpr const char* kSplitModels[] = {"1", "1.5", "2", "3", "4"};
int SplitQuality() {
  const auto& value = REXCVAR_GET(bo2zm_splitscreen_preset);
  for (int i = 0; i < int(std::size(kSplitQuality)); ++i) {
    if (value == kSplitQuality[i]) return i;
  }
  return 0;
}
void SetSplitQuality(int value) {
  rex::cvar::SetFlagByName("bo2zm_splitscreen_preset", kSplitQuality[value]);
}
int IpakRate() {
  const int value = REXCVAR_GET(bo2zm_ipak_mb_s_in_match);
  return value == 4 ? 0 : value == 16 ? 1 : 2;
}
void SetIpakRate(int value) {
  rex::cvar::SetFlagByName("bo2zm_ipak_mb_s_in_match", kIpakRates[value]);
}
int SplitModel() {
  const double value = REXCVAR_GET(bo2zm_splitscreen_model_scale);
  for (int i = 0; i < int(std::size(kSplitModels)); ++i) {
    if (value == std::atof(kSplitModels[i])) return i;
  }
  return 0;
}
void SetSplitModel(int value) {
  rex::cvar::SetFlagByName("bo2zm_splitscreen_model_scale", kSplitModels[value]);
}
constexpr const char* kTextureMemory[] = {"384", "768", "1536", "3072"};
int TextureMemory() {
  const int value = rex::cvar::Query<int32_t>("texture_cache_memory_limit_soft");
  for (int i = 0; i < int(std::size(kTextureMemory)); ++i) {
    if (value == std::atoi(kTextureMemory[i])) return i;
  }
  return 2;
}
void SetTextureMemory(int value) {
  const int soft = std::atoi(kTextureMemory[value]);
  rex::cvar::SetFlagByName("texture_cache_memory_limit_soft", kTextureMemory[value]);
  rex::cvar::SetFlagByName("texture_cache_memory_limit_hard", std::to_string(soft * 2));
}
void SetLoadingVideo(int on) {
  rex::cvar::SetFlagByName("bo2zm_loading_video", on ? "true" : "false");
}
// CPU CORES (Extras > EXPERIMENTAL; bo2zm_switch.cpp): 0 = 3 cores, 1 = 4. IN USE, read-only: 0 = 3 cores, 1 = 4,
// 2 = 3 because CPU CORES asks for 4 on a launch without core 3, 3 = 3 because the failsafe switched them off.
int CpuCores() {
  // After the failsafe the setting reads (and is saved as) 3: picking 4 tries again, and the next start does not.
  return bo2zm::CpuCoresSetting() == 4 && !bo2zm::CoresSwitchedOff() ? 1 : 0;
}
void SetCpuCores(int four) {
  bo2zm::SetCpuCoresSetting(four ? 4 : 3);
}
int CoresInUse() {
  if (bo2zm::CoresInUse() == 4) {
    return 1;
  }
  if (bo2zm::CoresSwitchedOff()) {
    return 3;
  }
  return bo2zm::CpuCoresSetting() == 4 && bo2zm::ProcessCores() != 4 ? 2 : 0;
}
void SetCoresInUse(int) {}  // it only shows what the game does

int God() {
  return bo2zm::cheats::GodMode();
}
void SetGod(int on) {
  bo2zm::cheats::SetGodMode(on);
}
// Noclip and UFO are one fly mode: turning one on turns the other off.
int Noclip() {
  return bo2zm::cheats::FlyMode() == kNoclip;
}
void SetNoclip(int on) {
  if (on || bo2zm::cheats::FlyMode() == kNoclip) {
    bo2zm::cheats::SetFlyMode(on ? kNoclip : kFlyOff);
  }
}
int Ufo() {
  return bo2zm::cheats::FlyMode() == kUfo;
}
void SetUfo(int on) {
  if (on || bo2zm::cheats::FlyMode() == kUfo) {
    bo2zm::cheats::SetFlyMode(on ? kUfo : kFlyOff);
  }
}
int Combo() {
  return bo2zm::cheats::Combo();
}
void SetCombo(int on) {
  bo2zm::cheats::SetCombo(on);
}
int ComboMode() {
  return bo2zm::cheats::ComboMode() == kUfo;
}
void SetComboMode(int ufo) {
  bo2zm::cheats::SetComboMode(ufo ? kUfo : kNoclip);
}
int Collision() {
  return bo2zm::cheats::FlyCollision();
}
void SetCollision(int on) {
  bo2zm::cheats::SetFlyCollision(on);
}
int Jump() {
  return bo2zm::cheats::JumpLevel();
}
void SetJump(int level) {
  bo2zm::cheats::SetJumpLevel(level);
}
int HudHidden() {
  return bo2zm::cheats::HudHidden();
}
void SetHudHidden(int hidden) {
  bo2zm::cheats::SetHudHidden(hidden);
}
int ThirdPerson() {
  return bo2zm::cheats::ThirdPerson();
}
void SetThirdPerson(int on) {
  bo2zm::cheats::SetThirdPerson(on);
}
int ThirdCrosshair() {
  return bo2zm::cheats::ThirdPersonCrosshairSetting();
}
void SetThirdCrosshair(int mode) {
  bo2zm::cheats::SetThirdPersonCrosshair(mode);
}
int CrosshairType() {
  return bo2zm::cheats::CrosshairType();
}
void SetCrosshairType(int type) {
  bo2zm::cheats::SetCrosshairType(type);
}
int CrosshairColor() {
  return bo2zm::cheats::CrosshairColor();
}
void SetCrosshairColor(int color) {
  bo2zm::cheats::SetCrosshairColor(color);
}
int Explosive() {
  return bo2zm::cheats::ExplosiveBullets();
}
void SetExplosive(int on) {
  bo2zm::cheats::SetExplosiveBullets(on);
}
int ZombiesIgnore() {
  return bo2zm::cheats::ZombiesIgnore();
}
void SetZombiesIgnore(int on) {
  bo2zm::cheats::SetZombiesIgnore(on);
}
// The round picker (ROUND in the menu's Zombies page): which round, applied by bo2zm_goto_round. Named "r<round>"
// so the menu's index ("bo2zm_round 4") is never read as a name.
constexpr const char* kRounds[] = {"r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r35", "r40", "r45", "r50", "r55", "r60", "r65", "r70", "r75", "r80", "r85", "r90", "r95", "r100", "r110", "r120", "r130", "r140", "r150", "r160", "r170", "r180", "r190", "r200", "r210", "r220", "r230", "r240", "r250", "r255"};
int g_round_choice = 0;
std::atomic<bool> g_changed{false};  // a menu command just ran
constexpr int kCompareEveryFrames = 8;
int RoundChoice() {
  return g_round_choice;
}
void SetRoundChoice(int index) {
  g_round_choice = index;
}
int KeepPerks() {
  return bo2zm::cheats::KeepPerksOn();
}
void SetKeepPerks(int on) {
  bo2zm::cheats::SetKeepPerks(on);
}
// The frame rate cap (FPS CAP in the game's graphics settings): PaceFrame waits out the rest of each frame.
constexpr const char* kFpsCaps[] = {"30", "60"};
constexpr int kFpsCapValues[] = {30, 60};
std::atomic<int> g_fps_cap{1};  // 60: no wait
std::atomic<int> g_vsync{0};
int FpsCap() {
  return g_fps_cap.load(std::memory_order_relaxed);
}
// The presentation interval for VSYNC and the cap (see the top of the file).
void ApplyVsync() {
#if REX_PLATFORM_SWITCH
  const bool on = g_vsync.load(std::memory_order_relaxed) != 0;
  RexSwitchPerfSwapIntervalForce(on ? (kFpsCapValues[g_fps_cap.load(std::memory_order_relaxed)] <= 30 ? 2 : 1) : 0);
#endif
}
void SetFpsCap(int index) {
  g_fps_cap.store(std::clamp(index, 0, int(std::size(kFpsCapValues)) - 1), std::memory_order_relaxed);
  ApplyVsync();
}
int Vsync() {
  return g_vsync.load(std::memory_order_relaxed);
}
void SetVsync(int on) {
  g_vsync.store(on ? 1 : 0, std::memory_order_relaxed);
  ApplyVsync();
}

// Options > Graphics DISPLAY (Mac/PC only): full screen or a window. The fullscreen cvar's change callback
// (sdk/src/ui/rex_app.cpp) resizes the window at once.
int WindowMode() {
  return REXCVAR_GET(fullscreen) ? 0 : 1;
}
void SetWindowMode(int window) {
  rex::cvar::SetFlagByName("fullscreen", window ? "false" : "true");
}

// Options KEYBOARD AND MOUSE (Mac/PC only): the SDK's keyboard/mouse driver (sdk/src/input/mnk) acts as player 1's
// controller; the mouse turns the camera while the game window has focus.
int KeyboardMouse() {
  return REXCVAR_GET(mnk_mode) ? 1 : 0;
}
void SetKeyboardMouse(int on) {
  // PC-style keys on top of the SDK's defaults: mouse buttons fire and aim, R reload, C crouch, G grenade, V melee,
  // 2 weapon swap. A bind changed in bo2zm.toml (no longer the SDK default) is left alone.
  struct Bind {
    const char* cvar;
    const char* sdk_default;
    const char* pc;
  };
  static constexpr Bind kPcBinds[] = {
      {"keybind_right_trigger", "E,O", "LMB,E,O"},
      {"keybind_left_trigger", "Q,I", "RMB,Q,I"},
      {"keybind_x", "L", "R,L"},
      {"keybind_b", "Quote,Backspace", "C,Quote,Backspace"},
      {"keybind_right_shoulder", "3", "G,3"},
      {"keybind_rstick_press", "K", "V,K"},
      {"keybind_y", "P", "2,P"},
  };
  if (on) {
    for (const Bind& bind : kPcBinds) {
      if (rex::cvar::GetFlagByName(bind.cvar) == bind.sdk_default) {
        rex::cvar::SetFlagByName(bind.cvar, bind.pc);
      }
    }
  }
  rex::cvar::SetFlagByName("mnk_mode", on ? "true" : "false");
  rex::cvar::SetFlagByName("mnk_mouse", on ? "true" : "false");
}

// Which rows the menu shows: the Switch-only ones (CPU CORES) or the Mac/PC-only ones (DISPLAY, KEYBOARD AND MOUSE).
int Platform() {
#if defined(__SWITCH__)
  return 0;
#else
  return 1;
#endif
}
void SetPlatform(int) {}

// Waits until when (see the top of the file). Main thread.
void WaitUntil(std::chrono::steady_clock::time_point when) {
#if REX_PLATFORM_WIN32
  static const HANDLE timer = [] {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!handle) {
      REXLOG_WARN("[extras] FPS cap: no high resolution timer (Windows 10 1803 or later); the cap paces less evenly");
    }
    return handle;
  }();
  if (timer) {
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) {
      return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -std::max<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100, 1);
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
        WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
      return;
    }
  }
#endif
  std::this_thread::sleep_until(when);
}
// SHOW PRESET (Options > Graphics): the preset stays in the top-right corner (bo2zm_preset.cpp).
int ShowPreset() {
  return REXCVAR_GET(bo2zm_preset_on_screen);
}
void SetShowPreset(int on) {
  rex::cvar::SetFlagByName("bo2zm_preset_on_screen", on ? "true" : "false");
}
int StopSpawns() {
  return bo2zm::cheats::StopSpawns();
}
void SetStopSpawns(int on) {
  bo2zm::cheats::SetStopSpawns(on);
}
int NoRecoil() {
  return bo2zm::cheats::NoRecoil();
}
void SetNoRecoil(int on) {
  bo2zm::cheats::SetNoRecoil(on);
}
int Ammo() {
  return bo2zm::cheats::InfiniteAmmo();
}
void SetAmmo(int on) {
  bo2zm::cheats::SetInfiniteAmmo(on);
}
int Fov() {
  return bo2zm::cheats::FieldOfView();
}
void SetFov(int level) {
  bo2zm::cheats::SetFieldOfView(level);
}
int Feather() {
  return bo2zm::cheats::FeatherFall();
}
void SetFeather(int on) {
  bo2zm::cheats::SetFeatherFall(on);
}
int GivePap() {
  return bo2zm::cheats::GivePackAPunch();
}
void SetGivePap(int on) {
  bo2zm::cheats::SetGivePackAPunch(on);
}
int CrossPerks() {
  return bo2zm::cheats::CrossMapPerks();
}
int MoveSpeed() {
  return bo2zm::cheats::MoveSpeed();
}
void SetMoveSpeed(int level) {
  bo2zm::cheats::SetMoveSpeed(level);
}
void SetCrossPerks(int on) {
  bo2zm::cheats::SetCrossMapPerks(on);
}

constexpr const char* kOffOn[] = {"off", "on"};
constexpr const char* kDefaultModes[] = {"campaign", "multiplayer", "zombies"};
// DEFAULT GAMEMODE shows the mode that will start (the saved one, or the installed one that replaces it) and takes
// installed modes only; the menu offers no others (menus.lua).
int DefaultMode() {
  const auto saved = static_cast<bo2zm::launch::Mode>(bo2zm::launch::GetDefaultModeIndex());
  return static_cast<int>(bo2zm::launch::FirstInstalled(saved, bo2zm::ModesInstalled()));
}
void SetDefaultMode(int mode) {
  if (!bo2zm::ModesInstalled().Has(static_cast<bo2zm::launch::Mode>(mode))) {
    REXLOG_WARN("[extras] default_mode: {} is not installed", kDefaultModes[mode]);
    return;
  }
  (void)bo2zm::launch::SetDefaultModeIndex(mode);
}
constexpr const char* kCoreCounts[] = {"3", "4"};
constexpr const char* kWindowModes[] = {"fullscreen", "window"};
constexpr const char* kPlatforms[] = {"switch", "pc"};
constexpr const char* kCoresInUse[] = {"3", "4", "3 (not a 4-core launch)", "3 (switched off by the failsafe)"};
constexpr const char* kFastFull[] = {"fast", "full"};
constexpr const char* kCachesOnOff[] = {"on", "off"};
constexpr const char* kLogDetails[] = {"normal", "detailed"};
constexpr const char* kMaxLogFiles[] = {"5", "10", "20", "off"};  // bo2zm::debug::MaxLogFiles
constexpr const char* kPurgeTiers[] = {"logs", "screenshots", "traces", "all"};
constexpr const char* kFlyModes[] = {"noclip", "ufo"};
constexpr const char* kJumpLevels[] = {"normal", "high", "higher", "highest", "insane", "extreme"};
constexpr const char* kMoveSpeeds[] = {"normal", "fast", "faster", "fastest"};
constexpr const char* kShownHidden[] = {"shown", "hidden"};
constexpr const char* kCrosshairModes[] = {"off", "aiming", "always"};
constexpr const char* kCrosshairTypes[] = {"dot", "cross"};
constexpr const char* kCrosshairColorNames[] = {"white", "red", "green", "cyan", "yellow", "magenta", "orange"};
constexpr const char* kFovs[] = {"55", "60", "65", "70", "75", "80", "85", "90", "95", "100", "105", "110"};
constexpr const char* kDebugPresets[] = {"custom", "basic", "verbose", "graphics", "performance", "crash", "full"};

// The settings that are cheats: with CHEATS off (bo2zm::cheats::MenuCheats) the menu cannot change them. The fly and
// crosshair preferences (collision, combo, its mode, the third person crosshair) are not cheats by themselves.
bool IsCheat(std::string_view name) {
  constexpr std::string_view kCheats[] = {"god",      "noclip",     "ufo",        "fov",   "ammo", "explosive",
                                          "norecoil", "zm_ignore",  "zm_nospawn", "keep_perks", "round", "jump",
                                          "feather",  "hud",        "third",      "speed"};
  return std::find(std::begin(kCheats), std::end(kCheats), name) != std::end(kCheats);
}

bool IsPlayerOption(std::string_view name) {
  constexpr std::string_view names[] = {
      "cheats", "god", "noclip", "ufo", "collision", "combo", "combo_mode", "fov", "ammo", "explosive",
      "norecoil", "zm_ignore", "keep_perks", "jump", "feather", "hud", "third", "third_crosshair",
      "tpp_type", "tpp_color", "give_pap", "cross_perks", "speed"};
  return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

bool IsWorldCommand(std::string_view name) {
  constexpr std::string_view names[] = {"bo2zm_hold", "bo2zm_release", "bo2zm_round", "bo2zm_goto_round",
      "bo2zm_powerup", "bo2zm_kill_zombies", "bo2zm_doors", "bo2zm_power", "bo2zm_debris", "bo2zm_zm_nospawn"};
  return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

bool IsPlayerAction(std::string_view name) {
  constexpr std::string_view names[] = {"bo2zm_perk", "bo2zm_remove_perks", "bo2zm_give_weapon",
      "bo2zm_pap_weapon", "bo2zm_max_points", "bo2zm_reset_cheats"};
  return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

bool ValidLocal(int local) { return local >= 0 && local < 4; }
bool PrimaryLocal(int local) { return ValidLocal(local) && local == bo2zm::cheats::PrimaryLocalPlayer(); }
int PlayerMaster(int local) {
  int value = 0;
  return ValidLocal(local) && bo2zm::cheats::GetPlayerOption(local, "cheats", value) ? value : 0;
}

void SetCheats(int on);

struct Option {
  const char* name;      // "bo2zm_<name>" is the command, "bo2zm_<name>_ui" the dvar the menu shows
  const char* saved_as;  // key in the settings file; nullptr: not kept between runs
  const char* const* values;
  int count;
  int (*get)();
  void (*set)(int);
};

constexpr Option kOptions[] = {
    {"split_quality", "split_quality", kSplitQuality, 6, SplitQuality, SetSplitQuality},
    {"ipak_rate", "ipak_rate", kIpakRates, 3, IpakRate, SetIpakRate},
    {"split_model", "split_model", kSplitModels, 5, SplitModel, SetSplitModel},
    {"texture_memory", "texture_memory", kTextureMemory, 4, TextureMemory, SetTextureMemory},
    {"default_mode", nullptr, kDefaultModes, 3, DefaultMode, SetDefaultMode},
    {"fps", "fps_counter", kOffOn, 2, FpsCounter, SetFpsCounter},
    {"loading_video", "loading_video", kOffOn, 2, LoadingVideo, SetLoadingVideo},
    {"cpu_cores", "cpu_cores", kCoreCounts, 2, CpuCores, SetCpuCores},
    {"cpu_cores_in_use", nullptr, kCoresInUse, 4, CoresInUse, SetCoresInUse},
    {"cheats", "cheats", kOffOn, 2, bo2zm::cheats::MenuCheats, SetCheats},
    {"dbg", "debug", kOffOn, 2, bo2zm::debug::Enabled, bo2zm::debug::SetEnabled},
    {"dbg_preset", "debug_preset", kDebugPresets, 7, bo2zm::debug::Preset, bo2zm::debug::SetPreset},
    {"mark_location", "mark_location", kOffOn, 2, bo2zm::debug::MarkLocation, bo2zm::debug::SetMarkLocation},
    {"god", nullptr, kOffOn, 2, God, SetGod},
    {"noclip", nullptr, kOffOn, 2, Noclip, SetNoclip},
    {"ufo", nullptr, kOffOn, 2, Ufo, SetUfo},
    {"collision", "fly_collision", kOffOn, 2, Collision, SetCollision},
    {"combo", "fly_combo", kOffOn, 2, Combo, SetCombo},
    {"combo_mode", "fly_combo_mode", kFlyModes, 2, ComboMode, SetComboMode},
    {"fov", "field_of_view", kFovs, 12, Fov, SetFov},
    {"ammo", nullptr, kOffOn, 2, Ammo, SetAmmo},
    {"explosive", nullptr, kOffOn, 2, Explosive, SetExplosive},
    {"norecoil", nullptr, kOffOn, 2, NoRecoil, SetNoRecoil},
    {"zm_ignore", nullptr, kOffOn, 2, ZombiesIgnore, SetZombiesIgnore},
    {"zm_nospawn", nullptr, kOffOn, 2, StopSpawns, SetStopSpawns},
    {"keep_perks", nullptr, kOffOn, 2, KeepPerks, SetKeepPerks},
    {"fps_cap", "fps_cap", kFpsCaps, 2, FpsCap, SetFpsCap},
    {"vsync", "vsync", kOffOn, 2, Vsync, SetVsync},
    {"window_mode", "window_mode", kWindowModes, 2, WindowMode, SetWindowMode},
    {"keyboard", "keyboard", kOffOn, 2, KeyboardMouse, SetKeyboardMouse},
    {"platform", nullptr, kPlatforms, 2, Platform, SetPlatform},
    // The DEBUG options' own picks are CUSTOM's, saved by bo2zm_debug_options.cpp (LoadCustom).
    {"dbg_textures", nullptr, kFastFull, 2, bo2zm::debug::TextureChecks,
     bo2zm::debug::SetTextureChecks},
    {"dbg_constants", nullptr, kFastFull, 2, bo2zm::debug::Constants, bo2zm::debug::SetConstants},
    {"dbg_caches", nullptr, kCachesOnOff, 2, bo2zm::debug::Caches, bo2zm::debug::SetCaches},
    {"dbg_safe", nullptr, kOffOn, 2, bo2zm::debug::SafeRenderer, bo2zm::debug::SetSafeRenderer},
    {"dbg_log", nullptr, kLogDetails, 2, bo2zm::debug::LogDetail, bo2zm::debug::SetLogDetail},
    {"dbg_profile", nullptr, kOffOn, 2, bo2zm::debug::StackProfile, bo2zm::debug::SetStackProfile},
    {"max_logs", "max_log_files", kMaxLogFiles, 4, bo2zm::debug::MaxLogFiles, bo2zm::debug::SetMaxLogFiles},
    {"purge_tier", nullptr, kPurgeTiers, 4, bo2zm::debug::PurgeTier, bo2zm::debug::SetPurgeTier},
    {"show_preset", "show_preset", kOffOn, 2, ShowPreset, SetShowPreset},
    {"round", nullptr, kRounds, int(std::size(kRounds)), RoundChoice, SetRoundChoice},
    {"jump", nullptr, kJumpLevels, 6, Jump, SetJump},
    {"speed", nullptr, kMoveSpeeds, 4, MoveSpeed, SetMoveSpeed},
    {"feather", nullptr, kOffOn, 2, Feather, SetFeather},
    {"hud", nullptr, kShownHidden, 2, HudHidden, SetHudHidden},
    {"third", nullptr, kOffOn, 2, ThirdPerson, SetThirdPerson},
    {"third_crosshair", "third_person_crosshair", kCrosshairModes, 3, ThirdCrosshair, SetThirdCrosshair},
    {"tpp_type", "tpp_crosshair_type", kCrosshairTypes, 2, CrosshairType, SetCrosshairType},
    {"tpp_color", "tpp_crosshair_color", kCrosshairColorNames, 7, CrosshairColor, SetCrosshairColor},
    {"give_pap", "give_pack_a_punch", kOffOn, 2, GivePap, SetGivePap},
    {"cross_perks", "cross_map_perks", kOffOn, 2, CrossPerks, SetCrossPerks},
};
constexpr size_t kCount = sizeof(kOptions) / sizeof(kOptions[0]);

// The core preserves personal picks through master-off. Only world-option picks
// are parked here, and only the primary player's switch can change them.
std::array<int, kCount> g_parked = [] {
  std::array<int, kCount> parked;
  parked.fill(-1);
  return parked;
}();

void SetCheats(int on) {
  const int local = bo2zm::cheats::PrimaryLocalPlayer();
  if (!ValidLocal(local) || (on != 0) == (PlayerMaster(local) != 0)) return;
  if (!on) for (size_t i = 0; i < kCount; ++i) {
    if (IsCheat(kOptions[i].name) && !IsPlayerOption(kOptions[i].name) &&
        std::string_view(kOptions[i].name) != "round") g_parked[i] = kOptions[i].get();
  }
  bo2zm::cheats::SetPlayerOption(local, "cheats", on);
  if (on) for (size_t i = 0; i < kCount; ++i) {
    if (g_parked[i] < 0) continue;
    kOptions[i].set(g_parked[i]);
    g_parked[i] = -1;
  }
}

int PlayerValue(int local, size_t index) {
  const auto& option = kOptions[index];
  if (IsPlayerOption(option.name)) {
    int value = -1;
    if (!ValidLocal(local) || !bo2zm::cheats::GetPlayerOption(local, option.name, value) ||
        value < 0 || value >= option.count) return -1;
    return value;
  }
  return g_parked[index] >= 0 ? g_parked[index] : option.get();
}

bool SetPlayerValue(int local, const Option& option, int value) {
  if (IsPlayerOption(option.name)) {
    if (!ValidLocal(local)) return false;
    if (std::string_view(option.name) == "cheats" && PrimaryLocal(local)) SetCheats(value);
    else return bo2zm::cheats::SetPlayerOption(local, option.name, value);
    return true;
  }
  if (ValidLocal(local) && !PrimaryLocal(local)) return false;
  option.set(value);
  return true;
}

// The value's index from its name (and true/false for off/on) or its index (the menu sends indexes; names come first,
// so a numeric name such as a field of view in degrees is read as the name); -1 if neither.
int Parse(const Option& option, std::string_view text) {
  for (int i = 0; i < option.count; ++i) {
    if (text == option.values[i]) {
      return i;
    }
  }
  if (option.values == kOffOn && (text == "true" || text == "false")) {
    return text == "true";
  }
  if (option.values == kFpsCaps && (text == "15" || text == "45")) {
    return text == "15" ? 0 : 1;  // the caps older builds saved
  }
  if (text.empty() || text.size() > 3 || text.find_first_not_of("0123456789") != std::string_view::npos) {
    return -1;
  }
  const int index = std::atoi(std::string(text).c_str());
  return index < option.count ? index : -1;
}

std::string PlayerPreference(int controller, const Option& option) {
  return "player." + std::to_string(controller) + "." + option.saved_as;
}

void LoadPlayerPreferences(int controller, int local) {
  for (const auto& option : kOptions) {
    if (!IsPlayerOption(option.name) || !option.saved_as) continue;
    auto saved = bo2zm::settings::Get(PlayerPreference(controller, option));
    if (!saved && PrimaryLocal(local)) saved = bo2zm::settings::Get(option.saved_as);
    if (!saved) continue;
    const int value = Parse(option, *saved);
    if (value >= 0) SetPlayerValue(local, option, value);
  }
}

void AppendUi(PPCContext& ctx, uint8_t* base, std::string& pending, std::string command) {
  if (!pending.empty() && pending.size() + command.size() + 2 > 1500) {
    bo2zm::autoexec::ExecuteCommands(ctx, base, pending);
    pending.clear();
  }
  if (!pending.empty()) pending += "; ";
  pending += command;
}

}  // namespace

// VSYNC's pacing (see the top of the file): the frame starts on the refresh 60 / cap refreshes after the last one's
// start, or at once when it is already that late. Main thread.
//
// The queue (bo2zm_vsync_drain): the window has three buffers, so with VSYNC up to two finished frames could wait for
// the screen, and once full the queue would stay full (frames made at the rate the screen takes them): about 67 ms
// more input delay at 30. The presenter waits for a free buffer only when two are queued, so when it waited since the
// last frame, this frame starts one refresh later; the queue loses half a frame each time until the waits stop. It is
// a guard: with the refresh signal an OLED console never queued (no presenter waits at all at 30).
//
// Late starts (bo2zm_vsync_late_start_ms): the frame that waited for its refresh starts a few ms after it. While the
// frames still finish in time (the same 30.0 FPS and no more frames that come after their refresh), each is shown on
// the same refresh as before and reads the controls that much later. A frame that came after its refresh starts at
// once, as before, and halves the delay; 60 frames in a row in time add 1 ms back, up to the setting. On an OLED, in
// Town on very low from round 20 at 30, 8 ms left no frame after its refresh in two minutes.
std::atomic<uint32_t> g_vsync_waits{0};  // frames after a presenter wait, for the report (main thread only)
uint32_t g_vsync_missed = 0;             // frames that came after their refresh (main thread only)
int g_vsync_late_ms = 0;                 // the late start now (main thread only)
static void PaceOnRefresh(int cap) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kRefresh = std::chrono::microseconds(16667);
  constexpr uint32_t kQueuedWaitUs = 2000;  // a dequeue wait longer than this: two frames were queued
  static Clock::time_point start{};  // when the last paced frame started
  auto due = start + (60 / cap) * kRefresh;
#if REX_PLATFORM_SWITCH
  static const bool drain = REXCVAR_GET(bo2zm_vsync_drain);
  if (RexSwitchPerfTakeDequeueWaitUs() > kQueuedWaitUs) {
    g_vsync_waits.store(g_vsync_waits.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (drain) {
      due += kRefresh;
    }
  }
  static Clock::time_point report{};
  if (Clock::now() - report >= std::chrono::seconds(10)) {
    if (report != Clock::time_point{}) {
      REXLOG_INFO("[extras] VSYNC at {}: in the last 10 s {} frames found that the presenter had waited for a free "
                  "buffer (two finished frames queued){}",
                  cap, g_vsync_waits.exchange(0, std::memory_order_relaxed),
                  drain ? "; each started a refresh later (bo2zm_vsync_drain)" : "");
      REXLOG_INFO("[extras] VSYNC at {}: in the last 10 s {} frames came after their refresh (late start now {} ms, "
                  "at most {})",
                  cap, g_vsync_missed, g_vsync_late_ms, REXCVAR_GET(bo2zm_vsync_late_start_ms));
      g_vsync_missed = 0;
    }
    report = Clock::now();
  }
#endif
  if (Clock::now() < due - std::chrono::milliseconds(1)) {
    // The refresh due is the first one at least half a refresh past the one before it (the 50 ms timeout only
    // matters if the signal stops, as in sleep).
    while (bo2zm::WaitForNextRefresh(50) && Clock::now() < due - kRefresh / 2) {
    }
    static const int late_max = std::clamp(REXCVAR_GET(bo2zm_vsync_late_start_ms), 0, 25);
    static int in_time = 0;
    if (++in_time >= 60 && g_vsync_late_ms < late_max) {
      ++g_vsync_late_ms;
      in_time = 0;
    }
    if (g_vsync_late_ms > 0) {
      const auto refresh = Clock::now();
      start = refresh;  // the schedule keeps to the refresh
      WaitUntil(refresh + std::chrono::milliseconds(g_vsync_late_ms));
      return;
    }
  } else {
    ++g_vsync_missed;
    g_vsync_late_ms /= 2;
  }
  start = Clock::now();
}

void bo2zm::extras::PaceFrame() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  int cap = kFpsCapValues[g_fps_cap.load(std::memory_order_relaxed)];
  bool vsync = g_vsync.load(std::memory_order_relaxed) != 0;
  // The campaign's front end has a cap of its own (bo2zm_front_end_fps_cap), paced by the timer below.
  const int front_end_cap = bo2zm::preset::FrontEnd() ? bo2zm::preset::FrontEndFpsCap() : 0;
  if (front_end_cap > 0) {
    cap = front_end_cap;
    vsync = false;
  } else if ((cap >= 60 && !vsync) || !bo2zm::cheats::InMatch()) [[likely]] {
    next = {};
    return;
  }
  if (vsync) {
    next = {};
    if (bo2zm::HasRefreshSignal()) {
      PaceOnRefresh(cap);
    }
    return;
  }
  const auto period = std::chrono::nanoseconds(1000000000 / cap);
  const auto now = Clock::now();
  if (next == Clock::time_point{} || now - next > period) {
    next = now;  // the first capped frame, or more than a frame behind: the schedule starts from here
  }
  next += period;
  if (next > now) {
    WaitUntil(next);
  }
}

bool bo2zm::extras::RunCommand(std::string_view name, std::string_view argument) {
  return RunPlayerCommand(bo2zm::cheats::PrimaryLocalPlayer(), name, argument);
}

bool bo2zm::extras::RunPlayerCommand(int local, std::string_view name, std::string_view argument) {
  constexpr std::string_view kPrefix = "bo2zm_";
  if (name.substr(0, kPrefix.size()) != kPrefix) {
    return false;
  }
  const auto full_name = name;
  name.remove_prefix(kPrefix.size());
  g_changed.store(true, std::memory_order_relaxed);
  if (!bo2zm::mode::IsZombies() && (IsPlayerOption(name) || IsWorldCommand(full_name) || IsPlayerAction(full_name)))
    return true;
  if (IsWorldCommand(full_name) && !PrimaryLocal(local)) {
    REXLOG_WARN("[extras] {} requires the primary local player", full_name);
    return true;
  }
  if (name == "reset_cheats") {
    if (!ValidLocal(local)) return true;
    if (PrimaryLocal(local)) g_parked.fill(-1);
    return bo2zm::cheats::RunPlayerCommand(local, full_name, argument);
  }
  if ((name == "goto_round" || IsCheat(name)) && !PlayerMaster(local)) {
    REXLOG_INFO("[extras] bo2zm_{} ignored: CHEATS is off", name);
    return true;
  }
  if (name == "goto_round") {
    bo2zm::cheats::GoToRound(std::atoi(kRounds[g_round_choice] + 1));
    return true;
  }
  if ((!ValidLocal(local) || PrimaryLocal(local)) && bo2zm::debug::RunCommand(std::string(full_name), argument)) {
    return true;
  }
  for (size_t i = 0; i < kCount; ++i) {
    const Option& option = kOptions[i];
    if (name != option.name) {
      continue;
    }
    if (!IsPlayerOption(name) && ValidLocal(local) && !PrimaryLocal(local)) return true;
    const int value = Parse(option, argument);
    if (value < 0) {
      REXLOG_WARN("[extras] bo2zm_{}: unknown value {}", option.name, argument);
    } else if (value != PlayerValue(local, i)) {
      REXLOG_INFO("[extras] {} {}", option.name, option.values[value]);
      SetPlayerValue(local, option, value);
    }
    return true;
  }
  return ValidLocal(local) && bo2zm::cheats::RunPlayerCommand(local, full_name, argument);
}

void bo2zm::extras::EveryFrame(PPCContext& ctx, uint8_t* base) {
  bo2zm::scripts::EveryFrame(ctx, base);
  static int shown[kCount];
  static bool started = false;
  static int frames = 0;
  static std::array<bool, 4> loaded{};
  static std::array<int, 4> mapped = {-2, -2, -2, -2};
  static auto player_shown = [] {
    std::array<std::array<int, kCount>, 4> result;
    for (auto& values : result) values.fill(-2);
    return result;
  }();
  // The settings change through the menu's commands (which mark g_changed) and a few program paths (the fly combo,
  // the cheats reset); every 8th frame catches those.
  if (started && !g_changed.load(std::memory_order_relaxed) && ++frames < kCompareEveryFrames) [[likely]] {
    return;
  }
  frames = 0;
  g_changed.store(false, std::memory_order_relaxed);
  if (!started) [[unlikely]] {
    started = true;
    bo2zm::cheats::SetGodMode(REXCVAR_GET(bo2zm_test_god));
    if (REXCVAR_GET(bo2zm_test_ignore)) {
      bo2zm::cheats::SetZombiesIgnore(true);
    }
    if (REXCVAR_GET(bo2zm_test_ammo)) {
      bo2zm::cheats::SetInfiniteAmmo(true);
    }
    // The DEBUG page's info line, and the installed modes: the menus offer only those (the home menu's Back popup,
    // DEFAULT GAMEMODE, the campaign's mode menu).
    bo2zm::autoexec::ExecuteCommands(ctx, base, "set bo2zm_build_ui \"" + bo2zm::debug::BuildText() +
                                                    "\"; set bo2zm_log_ui \"" + bo2zm::debug::LogFileName() +
                                                    "\"; set bo2zm_modes_ui \"" +
                                                    bo2zm::ModesInstalled().Text() +
                                                    "\"; set bo2zm_campaign_ui " +
                                                    (bo2zm::CampaignAvailable() ? "1" : "0") +
                                                    "; set bo2mp_local_camos_ui " +
                                                    (LocalCamoAssetsReady() ? "1" : "0"));
    REXLOG_INFO("[extras] Local MP camo assets {}", LocalCamoAssetsReady() ? "ready" : "unavailable");
    ApplyVsync();
    if (const std::string& start = REXCVAR_GET(bo2zm_test_start);
        !start.empty() && start.find_first_of("\";") == std::string::npos) {
      bo2zm::autoexec::ExecuteCommands(ctx, base, "set bo2zm_test_start_ui \"" + start + "\"; set "
                                                  "bo2zm_test_start_wait_ui " +
                                                  std::to_string(REXCVAR_GET(bo2zm_test_start_wait_s)) +
                                                  "; set bo2zm_test_lobby_only " +
                                                  std::to_string(REXCVAR_GET(bo2zm_test_lobby_only) ? 1 : 0));
      REXLOG_INFO("[extras] test start: {} (from the first game lobby)", start);
    }
    bo2zm::debug::LoadCustom();  // CUSTOM's picks, before a saved "debug_preset = custom" applies them
    for (size_t i = 0; i < kCount; ++i) {
      const Option& option = kOptions[i];
      shown[i] = -1;
      if (!option.saved_as || IsPlayerOption(option.name)) {
        continue;
      }
      // LOADING SCREEN VIDEO, CPU CORES and MAX LOG FILES set in bo2zm.toml (a test run) keep that value.
      if ((std::string_view(option.name) == "loading_video" &&
           rex::cvar::GetFlagSource("bo2zm_loading_video") != rex::cvar::Source::kDefault) ||
          (std::string_view(option.name) == "cpu_cores" &&
           rex::cvar::GetFlagSource("bo2zm_cpu_cores") != rex::cvar::Source::kDefault) ||
          (std::string_view(option.name) == "max_logs" &&
           rex::cvar::GetFlagSource("bo2zm_max_log_files") != rex::cvar::Source::kDefault)) {
        continue;
      }
      if (const auto saved = bo2zm::settings::Get(option.saved_as)) {
        const int value = Parse(option, *saved);
        if (value >= 0 && value != option.get()) {
          option.set(value);
          REXLOG_INFO("[extras] {} {}, from {}", option.name, option.values[value],
                      bo2zm::settings::File().string());
        }
      }
    }
    bo2zm::debug::Sync();  // the DEBUG switch and its picks as read (log level, texture checks, the profile's flag)
  }

  for (int controller = 0; controller < 4; ++controller) {
    const int local = bo2zm::cheats::LocalPlayerForController(controller);
    if (mapped[controller] != local) {
      mapped[controller] = local;
      player_shown[controller].fill(-2);
    }
    if (bo2zm::mode::IsZombies() && ValidLocal(local) && !loaded[controller]) {
      LoadPlayerPreferences(controller, local);
      loaded[controller] = true;
    }
  }
  std::string commands;
  const int primary = bo2zm::cheats::PrimaryLocalPlayer();
  for (size_t i = 0; i < kCount; ++i) {
    const Option& option = kOptions[i];
    const int value = PlayerValue(primary, i);
    if (value == shown[i]) [[likely]] {
      continue;
    }
    if (shown[i] >= 0 && option.saved_as && !IsPlayerOption(option.name) && value >= 0 && value < option.count) {
      bo2zm::settings::Set(option.saved_as, option.values[value]);
    }
    shown[i] = value;
    AppendUi(ctx, base, commands, "set bo2zm_" + std::string(option.name) + "_ui " + std::to_string(value));
  }
  for (int controller = 0; controller < 4; ++controller) {
    const int local = mapped[controller];
    for (size_t i = 0; i < kCount; ++i) {
      const auto& option = kOptions[i];
      if (!IsPlayerOption(option.name) && !IsCheat(option.name)) continue;
      const int value = bo2zm::mode::IsZombies() && ValidLocal(local) ? PlayerValue(local, i) : -1;
      const int before = player_shown[controller][i];
      if (before == value) continue;
      if (before >= 0 && value >= 0 && value < option.count && option.saved_as && IsPlayerOption(option.name)) {
        bo2zm::settings::Set(PlayerPreference(controller, option), option.values[value]);
        if (PrimaryLocal(local)) bo2zm::settings::Set(option.saved_as, option.values[value]);
      }
      player_shown[controller][i] = value;
      AppendUi(ctx, base, commands, "set bo2zm_p" + std::to_string(controller) + "_" + option.name + "_ui " +
                                    std::to_string(value));
    }
  }
  // PURGE DEBUG INFO's DELETE button (bo2zm_purge_ui): what the tier holds, or what it deleted.
  static std::string shown_purge;
  if (const std::string purge = bo2zm::debug::PurgeLabel(); purge != shown_purge) {
    shown_purge = purge;
    AppendUi(ctx, base, commands, "set bo2zm_purge_ui \"" + purge + "\"");
  }
  if (!commands.empty()) {
    bo2zm::autoexec::ExecuteCommands(ctx, base, commands);
  }
}
