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
// DLC map packs: packs whose license mask is exactly 1 (Nuketown Zombies).
//
// The game keeps one 76-byte record per mounted DLC package at 0x83880EF8 (8 records): +0 the content bit its id file
// names (dlczm0.id: 8), +64 the license mask XamContentCreateEx returned for it, +69 a byte set once it is mounted.
// sub_82412840(record) says whether a record may be used: it must be mounted, and when its license mask is exactly 1 it
// also needs the online bit (0x4) of the session mode flags at 0x837F8CB4 (sub_82406460). The game treats mask 1 as a
// trial or promotional license: it is only honoured in an online session (sub_82406470 sets or clears the bit when a
// session goes online or offline, sub_82406428 copies the flags from the server's game state), where an online content
// refresh (sub_82412E08) also sets +69 from the server's content_trialcontentpackbits dvar. An offline or local game
// never sets the bit. The package headers carry license bits 0x1 for Nuketown Zombies (and Nuketown 2025 and the Season
// Pass), 0x2/0x4/0x10/0x508 for the other packs, and the content manager reports those bits. So Nuketown mounted and
// showed on the globe, but starting it failed with Com_Error PLATFORM_MISSINGMAP (sub_82298CF8 -> sub_82412960 -> this
// check). The hook lets a mounted record with license mask 1 count as usable. It only changes a "no" for such a record,
// which the game gives only when the online bit is clear, and it keeps the mounted byte as the game left it.

#include "bo2zm_dlc.h"

#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "bo2zm_autoexec.h"

REXCVAR_DEFINE_BOOL(bo2zm_dlc_maps_seen, true, "BO2",
                    "Marks every Zombies map, start location and mode as already seen in each profile when it loads, "
                    "so installed DLC maps show no NEW badge on the globe");
REXCVAR_DEFINE_BOOL(bo2zm_dlc_license_one_offline, true, "BO2",
                    "Lets installed DLC packs whose license mask is 1 (Nuketown Zombies) be played offline; without "
                    "it the game wants a flag only an online service sends and starting the map fails");

namespace {

constexpr uint32_t kDlcRecords = 0x83880EF8;
constexpr uint32_t kDlcRecordSize = 76;
constexpr uint32_t kDlcRecordCount = 8;
constexpr uint32_t kDlcContentBit = 0;  // uint32, big-endian; dlczm0.id names bit 8
constexpr uint32_t kDlcLicenseMask = 64;  // uint32, big-endian
constexpr uint32_t kDlcMounted = 69;      // byte
constexpr uint32_t kSessionModeFlags = 0x837F8CB4;  // uint32, big-endian; sub_82406460 tests bit 0x4
constexpr uint32_t kSessionModeOnline = 0x4;

}  // namespace

REX_EXTERN(__imp__sub_82412840);
REX_HOOK_RAW(sub_82412840) {
  const uint32_t record = ctx.r3.u32;
  __imp__sub_82412840(ctx, base);
  if ((ctx.r3.u32 & 0xFF) != 0 || record >= kDlcRecordCount || !REXCVAR_GET(bo2zm_dlc_license_one_offline)) {
    return;
  }
  const uint8_t* entry = base + kDlcRecords + record * kDlcRecordSize;
  uint32_t mask;
  std::memcpy(&mask, entry + kDlcLicenseMask, sizeof(mask));
  uint32_t content_bit;
  std::memcpy(&content_bit, entry + kDlcContentBit, sizeof(content_bit));
  uint32_t session;
  std::memcpy(&session, base + kSessionModeFlags, sizeof(session));
  // Only the one refusal this is about: a mounted record, license mask exactly 1, and no online session (online, the
  // game decides by itself, through the server's content_trialcontentpackbits).
  if (entry[kDlcMounted] == 0 || __builtin_bswap32(content_bit) != 8 || __builtin_bswap32(mask) != 1 ||
      (__builtin_bswap32(session) & kSessionModeOnline) != 0) {
    return;
  }
  static bool logged[kDlcRecordCount] = {};
  if (!logged[record]) {
    logged[record] = true;
    REXLOG_INFO("[dlc] package {} (content bit {:X}) has license mask 1: usable offline", record,
                __builtin_bswap32(*reinterpret_cast<const uint32_t*>(entry)));
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
