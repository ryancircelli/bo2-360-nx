// DLC map packs: no NEW badge on the Zombies globe.
//
// The globe and start-location menus (Lua in the title update's patch_ui_zm.ff: SeenStartMap, SeenStartLocation,
// SeenGameModeMap, SawGameModeMap in SelectStartLocZombie) keep one bit per map, start location and mode in the
// profile setting unlock_crumbs_zm, and draw a NEW icon for every combination whose bit is clear; opening one sets its
// bit. Installed DLC maps (tools/install_dlc.py) therefore all start as NEW. Measured on the PC: with the setting at
// 16777215 (24 bits set) no map on the globe shows NEW, so every combination's bit is below bit 24.
//
// The profile setter behind the game's "setprofile" console command does nothing until that controller's profile is
// loaded: it first checks a byte at 0x836BAE58 + 2000 + controller * 2008 (one 2008-byte block per controller). So the
// per-frame hook watches that byte and, each time a profile finishes loading, runs "setprofile unlock_crumbs_zm
// 16777215" for that controller with the game's own text runner.
//
// DLC map packs: Nuketown Zombies (content bit 8) and Nuketown 2025 MP (content bit 4 in the map availability table).
//
// The game keeps one 76-byte record per mounted DLC package at 0x83880EF8 (8 records): +0 the availability content
// bit (Nuketown 2025 MP requests 4; Nuketown Zombies uses 8), +64 the license mask XamContentCreateEx returned for it,
// +69 a byte set once it is mounted.
// sub_82412840(record) says whether a record may be used: it must be mounted, and when its license mask is exactly 1 it
// also needs the online bit (0x4) of the session mode flags at 0x837F8CB4 (sub_82406460). The game treats mask 1 as a
// trial or promotional license: it is honoured when the online service supplies content_trialcontentpackbits, but an
// offline or local game never gets that bit. Both Nuketown packages therefore mount and appear in the menus but fail
// to start with Com_Error PLATFORM_MISSINGMAP (sub_82298CF8 -> sub_82412960 -> this check). The hook lets only those
// two mounted package records count as usable when this app's online link is idle or failed. It preserves the game's
// license check while the link is preparing or ready and leaves the mounted byte unchanged.

#include "bo2zm_dlc.h"

#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/online_link.h>

#include "bo2zm_autoexec.h"

REXCVAR_DEFINE_BOOL(bo2zm_dlc_maps_seen, true, "BO2",
                    "Marks every Zombies map, start location and mode as already seen in each profile when it loads, "
                    "so installed DLC maps show no NEW badge on the globe");
REXCVAR_DEFINE_BOOL(bo2zm_dlc_license_one_offline, true, "BO2",
                    "Lets installed Nuketown Zombies and Nuketown 2025 packs be played offline; without it the game "
                    "wants a flag only an online service sends and starting either map fails");

namespace {

constexpr uint32_t kDlcRecords = 0x83880EF8;
constexpr uint32_t kDlcRecordSize = 76;
constexpr uint32_t kDlcRecordCount = 8;
constexpr uint32_t kDlcContentBit = 0;  // uint32, big-endian
constexpr uint32_t kDlcLicenseMask = 64;  // uint32, big-endian
constexpr uint32_t kDlcMounted = 69;      // byte

uint32_t DlcRecordBit(const uint8_t* entry) {
  uint32_t bit;
  std::memcpy(&bit, entry + kDlcContentBit, sizeof(bit));
  return __builtin_bswap32(bit);
}

uint32_t DlcRecordLicense(const uint8_t* entry) {
  uint32_t mask;
  std::memcpy(&mask, entry + kDlcLicenseMask, sizeof(mask));
  return __builtin_bswap32(mask);
}

}  // namespace

REX_EXTERN(__imp__sub_82412840);

REX_HOOK_RAW(sub_82412840) {
  const uint32_t record = ctx.r3.u32;
  __imp__sub_82412840(ctx, base);
  if ((ctx.r3.u32 & 0xFF) != 0 || record >= kDlcRecordCount || !REXCVAR_GET(bo2zm_dlc_license_one_offline)) {
    return;
  }
  const uint8_t* entry = base + kDlcRecords + record * kDlcRecordSize;
  const uint32_t bit = DlcRecordBit(entry);
  // The app's online link state is authoritative for this port: preserve the server's license check while the app is
  // preparing for or hosting/joining an online session, and allow these two maps while the link is idle/failed.
  const auto online_state = rex::system::online::Current().state;
  const bool app_online = online_state == rex::system::online::State::kPreparing ||
                          online_state == rex::system::online::State::kReady;
  // Preserve the original bit-1 allowance, and include the traced MP map bit 4 alongside Zombies bit 8.
  if (entry[kDlcMounted] == 0 || (bit != 1 && bit != 4 && bit != 8) || DlcRecordLicense(entry) != 1 || app_online) {
    return;
  }
  static bool logged[kDlcRecordCount] = {};
  if (!logged[record]) {
    logged[record] = true;
    REXLOG_INFO("[dlc] package {} (content bit {:X}) has license mask 1: usable offline", record, bit);
  }
  ctx.r3.u64 = 1;
}

namespace {

constexpr uint32_t kProfileBlocks = 0x836BAE58;
constexpr uint32_t kProfileBlockSize = 2008;
constexpr uint32_t kProfileLoaded = 2000;  // byte: the profile of that controller is loaded
constexpr int kControllers = 4;
constexpr const char* kAllSeen = "setprofile unlock_crumbs_zm 16777215";

bool g_applied[kControllers] = {};

}  // namespace

void bo2zm::dlc::EveryFrame(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(bo2zm_dlc_maps_seen)) [[unlikely]] {
    return;
  }
  for (int controller = 0; controller < kControllers; ++controller) {
    const bool loaded =
        base[kProfileBlocks + uint32_t(controller) * kProfileBlockSize + kProfileLoaded] != 0;
    if (!loaded) {
      g_applied[controller] = false;  // signed out or not loaded yet: apply again on the next load
      continue;
    }
    if (g_applied[controller]) [[likely]] {
      continue;
    }
    g_applied[controller] = true;
    REXLOG_INFO("[dlc] profile of controller {} loaded: every Zombies map marked as seen (no NEW badges)", controller);
    bo2zm::autoexec::ExecuteCommands(ctx, base, kAllSeen, controller);
  }
}
