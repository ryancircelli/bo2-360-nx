// SPDX-License-Identifier: BSD-3-Clause
// Stubs for the xam/online modules (name entry, internet play, DLC scan) until the clean-room
// online APIs (PAS-48) land on v0.11. Each reports "not available" and does nothing per frame.
#include <cstdint>
#include <string_view>

#include <rex/ppc/context.h>

#include "../bo2zm_dlc.h"
#include "../bo2zm_name.h"

namespace bo2zm::name {
void ApplySaved() {}
void EveryFrame(PPCContext&, uint8_t*) {}
bool RunCommand(std::string_view, std::string_view) { return false; }
}  // namespace bo2zm::name

namespace bo2zm::dlc {
void EveryFrame(PPCContext&, uint8_t*) {}
}  // namespace bo2zm::dlc

// Relaunch (mode switch) is off on v0.11 until its kernel dependencies are ported: Zombies only, no restart.
#include <filesystem>
#include <vector>

#include <rex/xam/launch_data.h>

#include "../bo2zm_launch_options.h"
#include "../bo2zm_relaunch.h"

namespace bo2zm {
// The game starts Zombies when its launch data is 1000 bytes with [3] = 0x0B and [860] = 1 (captured from the
// game's own relaunch); the SDK hands this to XamLoaderGetLaunchData.
void ApplyPendingRelaunch(const std::filesystem::path&) {
  std::vector<uint8_t> data(1000, 0);
  data[3] = 0x0B;
  data[860] = 1;
  rex::xam::SetInitialLaunchData(std::move(data), 0, "game:\\default_mp.xex");
}
bool CampaignAvailable() { return false; }
const launch::InstalledModes& ModesInstalled() {
  static const launch::InstalledModes modes{};
  return modes;
}
void HandOverEarly() {}
void RequestRestart() {}
bool RestartForScriptChanges() { return false; }
}  // namespace bo2zm

// Cheat sync (online) and the wait-primitive self-test (needs v0.10 kernel threads): off on v0.11.
#include <functional>
#include <string>
#include "../bo2zm_cheat_sync.h"
#include "../bo2zm_test_waits.h"

namespace bo2zm::cheat_sync {
void EveryFrame(PPCContext&, uint8_t*) {}
bool Forward(int, std::string_view, std::string_view, bool) { return false; }
void TraceFrame(bool, bool) {}
}  // namespace bo2zm::cheat_sync

namespace bo2zm::test_waits {
bool Run(double, const std::function<void(const std::string&)>&) { return false; }
void RunIfRequested() {}
}  // namespace bo2zm::test_waits

// Link-time stand-ins for the removed relaunch module and SDK settings (v0.11).
#include <rex/cvar.h>
#include <rex/ui/image_decode.h>
#include "../bo2zm_launch_options.h"
#include "../bo2zm_mode.h"
namespace bo2zm::mode {
bool IsZombies() { return true; }
}  // namespace bo2zm::mode
namespace bo2zm::launch {
void RestartCurrentMode() {}
}  // namespace bo2zm::launch
// The SDK's old "fullscreen" and "mnk_mode" cvars are gone in v0.11; bo2zm_extras reads them through the vendored cvar lib.
REXCVAR_DEFINE_BOOL(fullscreen, false, "Graphics", "Fullscreen window (v0.10 compat)");
REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Keyboard and mouse mode (v0.10 compat)");
namespace rex::ui {
std::vector<uint8_t> DecodeImageRGBA(const uint8_t*, size_t, int& w, int& h) {
  w = h = 0;
  return {};  // v0.11 does not export the SDK's decoder; the credits images stay empty
}
}  // namespace rex::ui
