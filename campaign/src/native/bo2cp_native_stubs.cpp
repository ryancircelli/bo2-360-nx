// The Zombies program's systems that the native renderer and the menus' files call (games/bo2/src/native/render,
// bo2zm_d3d_marker.cpp, bo2zm_extras.cpp, bo2zm_debug_options.cpp), for the campaign: each gives the answer of the
// feature turned off, until the campaign has its own. The features behind
// them hook default_mp's functions (cheats, the lighting volume's writer, the benchmark) or draw the Zombies
// home screen (credits).
//
// The write-stamp caches (bo2zm_page_writes.cpp, built as it is) get their stamps from bo2zm_crt.cpp and the skinning
// hooks, as in the Zombies program.

#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include "bo2zm_benchmark.h"
#include "bo2zm_cheats.h"
#include "bo2zm_mode.h"
#include "bo2zm_credits.h"
#include "bo2zm_updates.h"
#include "bo2zm_debug_options.h"
#include "bo2zm_debug_watch.h"
#include "bo2zm_preset.h"
#include "bo2zm_relaunch.h"
#include "bo2zm_switch.h"
#include "bo2zm_volume_writes.h"
#include "native/render/bo2zm_native_render.h"

namespace bo2zm {
bool CampaignAvailable() { return false; }  // the Zombies home menu's Back popup (bo2zm_relaunch.cpp); this is the campaign
}  // namespace bo2zm

namespace bo2zm::mode {
bool IsZombies() { return false; }
}  // namespace bo2zm::mode

namespace bo2zm::benchmark {
// The renderer reports every frame it presents here; the campaign's watchdog counts them (bo2cp_relaunch.cpp).
std::atomic<uint64_t> g_frames_seen{0};
void AnnotateFrame() { g_frames_seen.fetch_add(1, std::memory_order_relaxed); }
uint64_t FramesSeen() { return g_frames_seen.load(std::memory_order_relaxed); }
int AbArm() { return -1; }  // no A/B running
int AbArmCached() { return -1; }
bool IsActive() { return false; }  // no benchmark route in the campaign (bo2zm_test_pad.cpp asks)
bool PathTilts(int&, int&) { return false; }
uint32_t PathButtons() { return 0; }
void ApplyOwnPriority() {}  // the benchmark's own thread priority (bo2zm_waits.cpp asks)
}  // namespace bo2zm::benchmark

// The Zombies program's cheats (bo2zm_cheats.cpp hooks default_mp's players, rounds, perks and weapons): none in the
// campaign. The Extras menu leaves their pages out there; its settings table and the DEBUG page still ask.
namespace bo2zm::cheats {
int LocalPlayerForController(int) { return -1; }
int PrimaryLocalPlayer() { return -1; }
bool IsPrimaryController(int) { return false; }
bool GetPlayerOption(int, std::string_view, int&) { return false; }
bool SetPlayerOption(int, std::string_view, int) { return false; }
bool RunPlayerCommand(int, std::string_view, std::string_view) { return false; }
uint16_t OnPad(const bo2zm::pad::Pad&, int) { return 0; }
int GetCrosshairOverlays(CrosshairOverlay*, int) { return 0; }
void BeginMapLoad() {}
void EndMapLoad() {}
int JumpLevelForState(const uint8_t*, uint32_t) { return 0; }
float FieldOfViewScaleForLocal(int) { return 1.0f; }
bool RunCommand(std::string_view, std::string_view) { return false; }
uint16_t OnPad(const bo2zm::pad::Pad&) { return 0; }  // no cheat buttons in the campaign
bool ThirdPersonCrosshair() { return false; }
int CrosshairType() { return 0; }
int CrosshairColor() { return 0; }
void SetCrosshairType(int) {}
void SetCrosshairColor(int) {}
int MenuCheats() { return 0; }
void SetMenuCheats(int) {}
bool GodMode() { return false; }
void SetGodMode(bool) {}
int FlyMode() { return kFlyOff; }
void SetFlyMode(int) {}
bool FlyCollision() { return true; }
void SetFlyCollision(bool) {}
bool Combo() { return false; }
void SetCombo(bool) {}
int ComboMode() { return kNoclip; }
void SetComboMode(int) {}
int JumpLevel() { return 0; }
void SetJumpLevel(int) {}
bool HudHidden() { return false; }
void SetHudHidden(bool) {}
bool ThirdPerson() { return false; }
void SetThirdPerson(bool) {}
int ThirdPersonCrosshairSetting() { return 0; }
void SetThirdPersonCrosshair(int) {}
bool FeatherFall() { return false; }
void SetFeatherFall(bool) {}
bool InfiniteAmmo() { return false; }
void SetInfiniteAmmo(bool) {}
bool ExplosiveBullets() { return false; }
void SetExplosiveBullets(bool) {}
bool NoRecoil() { return false; }
void SetNoRecoil(bool) {}
bool ZombiesIgnore() { return false; }
void SetZombiesIgnore(bool) {}
bool StopSpawns() { return false; }
void SetStopSpawns(bool) {}
void GoToRound(int) {}
bool KeepPerksOn() { return false; }
void SetKeepPerks(bool) {}
bool GivePackAPunch() { return false; }
void SetGivePackAPunch(bool) {}
bool CrossMapPerks() { return false; }
void SetCrossMapPerks(bool) {}
int CurrentRound() { return 0; }
bool PlayerSpot(Spot&) { return false; }  // MARK LOCATION saves no position in the campaign
int FieldOfView() { return 0; }
void SetFieldOfView(int) {}
int MoveSpeed() { return 0; }
void SetMoveSpeed(int) {}
}  // namespace bo2zm::cheats

// Internet play (bo2zm_online.cpp) belongs to the Zombies and Multiplayer program; the campaign has no lobby for it.
namespace bo2zm::online {
bool RunCommand(std::string_view, std::string_view) { return false; }
}  // namespace bo2zm::online

namespace bo2zm::credits {
void SetShown(bool) {}
const Picture* Images() { return nullptr; }
bool Place(uint32_t, uint32_t, Placement&) { return false; }  // no credits line over the campaign's menus
}  // namespace bo2zm::credits

namespace bo2zm::debug {
void WatchPhysicalPages(const std::vector<uint32_t>&, Watch, const std::vector<uint64_t>*) {}
bool WatchVertices() { return false; }
}  // namespace bo2zm::debug

namespace bo2zm::volume_writes {
bool UseMarks() { return false; }  // the renderer hashes the lighting volume instead
void TakeMarks(uint32_t, uint32_t, std::vector<uint32_t>&) {}
void Missed(uint32_t) {}
}  // namespace bo2zm::volume_writes

// CHECK FOR UPDATES is on the Zombies home menu only (bo2zm_commands.cpp still names it).
namespace bo2zm::updates {
void Command(std::string_view) {}
}  // namespace bo2zm::updates
