// The game's per-frame hook on the main thread, and the helpers that hang off it.
//
// sub_8240F058 is called once per frame by the game's main loop (it checks frame times and code_warning_fps, and
// raises deferred errors). Our hook paces the FPS cap there, gives every module that needs the main thread its
// per-frame call (presets, extras, the player name, the cheats, DLC, the UI, updates), and keeps the .ipak read rate in
// step with whether a match is on. It also runs the test helpers:
//   - bo2zm_autoexec runs console commands once, a few seconds after start-up, so a test run can reach a match without
//     anyone clicking through the menus:
//       --bo2zm_autoexec="cmd1; cmd2" --bo2zm_autoexec_delay_s=20
//     The commands go to sub_824032C0(client, controller, text), the text runner the "exec" command uses on a config
//     file's contents: it splits on ';' and new lines and runs each command (sub_82402E78). The text is placed in
//     unused guest stack space below the current frame and the stack pointer is moved past it for the call, so no
//     guest memory is allocated. Off by default.
//   - bo2zm_renderdoc_capture_s captures frames when the game runs under RenderDoc (PC only).
//   - bo2zm_dump_memory copies guest physical memory to files at given times.
// The sv_cheats registration hook further down makes the game's built-in cheats usable.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>

#include "bo2zm_autoexec.h"
#include "bo2zm_output_volume.h"
#include "bo2zm_benchmark.h"
#include "bo2zm_bots.h"
#include "bo2zm_cheats.h"
#include "bo2zm_controllers.h"
#include "bo2zm_dlc.h"
#include "bo2zm_dvars.h"
#include "bo2zm_extras.h"
#include "bo2zm_heap_track.h"
#include "bo2zm_mode.h"
#include "bo2zm_name.h"
#include "bo2zm_ui.h"
#include "bo2zm_online.h"
#include "bo2zm_updates.h"
#include "bo2zm_preset.h"
#include "native/bo2zm_native_system.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#if REX_PLATFORM_WIN32
#include <rex/ui/renderdoc_api.h>
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(bo2zm_volume, 100, "BO2",
                     "PC output volume, 0-100 percent; Switch always uses normal 100 percent output")
    .range(0, 100);

// In a match the game's image streaming pool (125 MB, bounded by the 360's memory) is full, so the game evicts images
// and reads them back (99 % of its .ipak reads are re-reads), and each image it brings back is a new texture for the
// renderer. Limiting .ipak reads to 4 MB/s in a match, instead of the SDK's 32 (the game read about 6.4 MB/s), gave
// 4.3 % more frames per second on the console in an A/B comparison. Menus and loading keep the SDK's rate.
#if defined(__SWITCH__)
inline constexpr int32_t kDefaultIpakInMatch = 4;
#else
inline constexpr int32_t kDefaultIpakInMatch = 16;  // Mac: unlimited was worse on the gump texture check
#endif
REXCVAR_DEFINE_INT32(bo2zm_ipak_mb_s_in_match, kDefaultIpakInMatch, "BO2",
                     "The .ipak read rate limit (the SDK's switch_io_ipak_mb_s) while player 0 is in a match; menus and "
                     "loading keep the SDK's own value. 0 = always the SDK's")
    .range(0, 1024);
REXCVAR_DECLARE(int32_t, bo2zm_benchmark_ipak_mb_s_b);

REXCVAR_DEFINE_STRING(bo2zm_autoexec, "", "BO2",
                      "Test helper: console commands run once on the main thread after bo2zm_autoexec_delay_s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_autoexec_delay_s, 20, "BO2", "Test helper: seconds before bo2zm_autoexec runs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Test helper, PC only: with the game started under RenderDoc (renderdoccmd capture ...), capture one frame
// this many seconds after startup, to inspect the native renderer's draws. 0 = off.
REXCVAR_DEFINE_INT32(bo2zm_renderdoc_capture_s, 0, "BO2",
                     "Test helper (PC, under RenderDoc): capture one frame after this many seconds (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_renderdoc_count, 1, "BO2",
                     "Test helper (PC, under RenderDoc): how many frames to capture from bo2zm_renderdoc_capture_s on, "
                     "each one requested once the previous capture is written")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_renderdoc_path, "", "BO2",
                      "Test helper: RenderDoc capture file path template (empty = RenderDoc's default)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Test helper: copy guest physical memory to a file at given times, under any renderer (Xenos or native), to
// compare what the game has put at an address. "ADDR:BYTES@S1,S2,..." (hex address and size, seconds after
// startup); writes dump_<ADDR>_<S>s.bin next to the executable. Empty = off.
REXCVAR_DEFINE_STRING(bo2zm_dump_memory, "", "BO2",
                      "Test helper: \"ADDR:BYTES@S1,S2\" copies guest physical memory to dump_<ADDR>_<S>s.bin")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

std::chrono::steady_clock::time_point g_start{};
bool g_done = false;
bool g_capture_done = false;
int32_t g_captures_requested = 0;

#if REX_PLATFORM_WIN32
// The RenderDoc in-app API when the game runs under RenderDoc, with bo2zm_renderdoc_path set; null otherwise.
rex::ui::RenderDocAPI* RenderDoc() {
  static std::unique_ptr<rex::ui::RenderDocAPI> api = [] {
    auto api = rex::ui::RenderDocAPI::CreateIfConnected();
    const std::string& path = REXCVAR_GET(bo2zm_renderdoc_path);
    if (api && !path.empty()) {
      api->api_1_0_0()->SetLogFilePathTemplate(path.c_str());
    }
    return api;
  }();
  return api.get();
}
#endif

void MaybeTriggerCapture(std::chrono::steady_clock::time_point now) {
#if REX_PLATFORM_WIN32
  if (g_capture_done) [[likely]] {
    return;
  }
  const int32_t seconds = REXCVAR_GET(bo2zm_renderdoc_capture_s);
  if (seconds <= 0) {
    g_capture_done = true;
    return;
  }
  if (now - g_start < std::chrono::seconds(seconds)) {
    return;
  }
  rex::ui::RenderDocAPI* api = RenderDoc();
  if (!api) {
    g_capture_done = true;
    REXLOG_WARN("[renderdoc] not running under RenderDoc: no capture");
    return;
  }
  if (g_captures_requested > 0 && api->api_1_0_0()->GetNumCaptures() < uint32_t(g_captures_requested)) {
    return;  // the previous capture is not written yet
  }
  api->api_1_0_0()->TriggerCapture();
  ++g_captures_requested;
  g_capture_done = g_captures_requested >= std::max(1, REXCVAR_GET(bo2zm_renderdoc_count));
  REXLOG_INFO("[renderdoc] capture {} of the next frame requested", g_captures_requested);
#else
  (void)now;
#endif
}

// Physical memory is also mapped at guest 0xA0000000 (xmemory.cpp heap vA0000000), so base + 0xA0000000 + addr.
void MaybeDumpMemory(std::chrono::steady_clock::time_point now, uint8_t* base) {
  static size_t next = 0;  // index of the next time in the list
  const std::string& spec = REXCVAR_GET(bo2zm_dump_memory);
  const size_t colon = spec.find(':'), at = spec.find('@');
  if (spec.empty() || colon == std::string::npos || at == std::string::npos) {
    return;
  }
  size_t pos = at + 1;
  for (size_t i = 0; i < next && pos != std::string::npos; ++i) {
    pos = spec.find(',', pos);
    pos = pos == std::string::npos ? pos : pos + 1;
  }
  if (pos == std::string::npos || pos >= spec.size()) {
    return;
  }
  const uint32_t seconds = uint32_t(std::strtoul(spec.c_str() + pos, nullptr, 10));
  if (now - g_start < std::chrono::seconds(seconds)) {
    return;
  }
  ++next;
  const uint32_t address = uint32_t(std::strtoul(spec.c_str(), nullptr, 16)) & 0x1FFFFFFF;
  const uint32_t bytes = uint32_t(std::strtoul(spec.c_str() + colon + 1, nullptr, 16));
  if (!bytes || uint64_t(address) + bytes > 0x20000000) {
    return;
  }
  char name[64];
  std::snprintf(name, sizeof(name), "dump_%08X_%us.bin", address, seconds);
  const auto path = rex::filesystem::GetExecutableFolder() / name;
  std::ofstream out(path, std::ios::binary);
  // In 64 KB pieces: pages the guest never committed are not readable and are written as zeros.
  static char zeros[0x10000] = {};
  uint32_t skipped = 0;
  for (uint32_t done = 0; done < bytes; done += 0x10000) {
    const uint32_t n = std::min<uint32_t>(0x10000, bytes - done);
    const char* source = reinterpret_cast<const char*>(base + 0xA0000000u + address + done);
#if REX_PLATFORM_WIN32
    MEMORY_BASIC_INFORMATION info{};
    const bool readable = VirtualQuery(source, &info, sizeof(info)) && info.State == MEM_COMMIT &&
                          !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                          reinterpret_cast<const char*>(info.BaseAddress) + info.RegionSize >= source + n;
#else
    const bool readable = true;
#endif
    skipped += readable ? 0 : 1;
    out.write(readable ? source : zeros, std::streamsize(n));
  }
  REXLOG_INFO("[dump] memory {:08X}+{:X} written to {} ({} unreadable 64 KB pieces as zeros)", address, bytes,
              path.string(), skipped);
}

}  // namespace

// sv_cheats on and writable, so the game's built-in cheats work: the dvars with the cheat flag 0x80
// (player_sustainAmmo, cg_thirdPerson, jump_height, timescale, ...; tools/inspect/dvar_list.py --cheat lists them) can
// be changed from the Extras menu or the console. Retail TU18 has no cheat commands: there is no god or noclip handler,
// and the strings ufo, notarget and give are not in the executable; only these dvars remain. sub_82498550 registers a
// bool dvar (r3 name, r4 default, r5 flags, r6 description). sv_cheats is registered more than once (at 0x8243230C
// with flags 0x18, at 0x828B4154 with flags 0x48, and again on map loads), and "set sv_cheats 1" fails with
// "sv_cheats is read only" (a Com_Error that also ends the match). So each registration gets default 1 and loses
// flags 0x40 and 0x10, which are probably the read-only and init-only bits (DVAR_ROM 0x40 and DVAR_INIT 0x10 in the
// IW engines). With that, every registration logs "on, flags 8".
REXCVAR_DEFINE_BOOL(bo2zm_sv_cheats, true, "BO2",
                    "sv_cheats on and writable, so the game's cheat-protected dvars (its built-in cheats) can be "
                    "changed from the Extras menu or the console")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82498550);
REX_HOOK_RAW(sub_82498550) {
  const uint32_t name = ctx.r3.u32;
  if (bo2zm::mode::IsZombies() && REXCVAR_GET(bo2zm_sv_cheats) && ctx.r3.u32 &&
      std::strcmp(reinterpret_cast<const char*>(base + ctx.r3.u32), "sv_cheats") == 0) {
    REXLOG_INFO("[cheats] sv_cheats registered with flags {:X}: on, flags {:X}", ctx.r5.u32,
                ctx.r5.u32 & ~uint32_t(0x50));
    ctx.r4.u64 = 1;
    ctx.r5.u64 = ctx.r5.u32 & ~uint32_t(0x50);
  }
  __imp__sub_82498550(ctx, base);
  bo2zm::dvars::Registered(base, name, ctx.r3.u32, bo2zm::dvars::kBool);  // r3 = the dvar
  bo2zm::preset::DvarRegistered(base, name, ctx.r3.u32, 0);
}

namespace {
// bo2zm_ipak_mb_s_in_match: the SDK's .ipak rate follows whether player 0 is in a match (the benchmark's A/B factor,
// when it is set, drives the same rate and wins).
void ApplyStreamingRate() {
  const int32_t in_match = REXCVAR_GET(bo2zm_ipak_mb_s_in_match);
  static const bool ab = REXCVAR_GET(bo2zm_benchmark_ipak_mb_s_b) >= 0;
  if (ab) {
    return;
  }
  static const int32_t usual = rex::cvar::Query<int32_t>("switch_io_ipak_mb_s");
  static int applied = -1;
  const int playing = bo2zm::cheats::InMatch() ? 1 : 0;
  const int32_t desired = playing && in_match > 0 ? in_match : usual;
  if (desired == applied) {
    return;
  }
  applied = desired;
  rex::cvar::SetFlagByName("switch_io_ipak_mb_s", std::to_string(desired));
  REXLOG_INFO("[io] .ipak reads at most {} MB/s ({})", desired,
              playing && in_match > 0 ? "in a match, bo2zm_ipak_mb_s_in_match" : "menus, loading or unlimited");
}

// The benchmark's route starts once a match is loaded (player 0 has a client).
void AnnotateBenchmark(const uint8_t* base) {
  if (!bo2zm::benchmark::IsActive()) [[likely]] {
    return;
  }
  constexpr uint32_t kGEntities = 0x833D0640;
  constexpr uint32_t kEntClient = 340;
  uint32_t client;
  std::memcpy(&client, base + kGEntities + kEntClient, 4);
  bo2zm::benchmark::AnnotatePlayer(__builtin_bswap32(client) >= 0x82000000u);
  // The native renderer ticks the route at each Swap; with the emulated GPU (bo2zm_renderer = xenos, the PC's
  // reference for test spots) nothing would, so the game's frame does.
  if (!bo2zm::native::PortedRendererActive()) {
    bo2zm::benchmark::AnnotateFrame();
  }
}
}  // namespace

REX_EXTERN(__imp__sub_824032C0);
REX_EXTERN(__imp__sub_8240F058);

void bo2zm::autoexec::ApplyVolume() {
  const int volume = bo2zm::audio::OutputVolumePercent(REXCVAR_GET(bo2zm_volume));
  rex::audio::SetOutputGain(float(volume) / 100.0f);
  REXLOG_INFO("[autoexec] sound output volume {} %", volume);
}

bool bo2zm::autoexec::CaptureRenderDoc() {
#if REX_PLATFORM_WIN32
  rex::ui::RenderDocAPI* api = RenderDoc();
  if (!api) {
    REXLOG_WARN("[renderdoc] not running under RenderDoc: no capture");
    return false;
  }
  api->api_1_0_0()->TriggerCapture();
  REXLOG_INFO("[renderdoc] capture of the next frame requested");
  return true;
#else
  return false;
#endif
}

void bo2zm::autoexec::ExecuteCommands(PPCContext& ctx, uint8_t* base, std::string_view commands, int controller) {
  if (commands.size() > 2000) {
    REXLOG_WARN("[autoexec] commands too long ({} bytes): skipped", commands.size());
    return;
  }
  // Text in the guest stack, 4 KB below the current frame; the callee's frames go below that.
  const auto saved = ctx;
  const uint32_t text = (ctx.r1.u32 - 4096) & ~uint32_t(0xF);
  std::memcpy(base + text, commands.data(), commands.size());
  base[text + commands.size()] = 0;
  ctx.r1.u64 = (text - 256) & ~uint64_t(0xF);
  ctx.r3.u64 = 0;  // local client
  ctx.r4.u64 = uint64_t(controller);
  ctx.r5.u64 = text;
  __imp__sub_824032C0(ctx, base);
  ctx = saved;
}

REX_HOOK_RAW(sub_8240F058) {
  bo2zm::extras::PaceFrame();  // the FPS cap waits where the game's own frame rate limit does (bo2zm_extras.cpp)
  __imp__sub_8240F058(ctx, base);
  if (g_start.time_since_epoch().count() == 0) {
    g_start = std::chrono::steady_clock::now();
  }
  MaybeTriggerCapture(std::chrono::steady_clock::now());
  MaybeDumpMemory(std::chrono::steady_clock::now(), base);
  AnnotateBenchmark(base);
  bo2zm::heap_track::EveryFrame();  // test build only (BO2ZM_HEAP_TRACK)
  ApplyStreamingRate();
  bo2zm::preset::EveryFrame(ctx, base);
  bo2zm::extras::EveryFrame(ctx, base);
  bo2zm::name::EveryFrame(ctx, base);
  bo2zm::cheats::EveryFrame(ctx, base);
  bo2zm::controllers::EveryFrame();
  bo2zm::dlc::EveryFrame(ctx, base);
  bo2zm::ui::EveryFrame(ctx, base);
  bo2zm::bots::EveryFrame(ctx, base);
  bo2zm::updates::EveryFrame(ctx, base);
  bo2zm::online::EveryFrame(ctx, base);
  if (g_done) [[likely]] {
    return;
  }
  const std::string& commands = REXCVAR_GET(bo2zm_autoexec);
  if (commands.empty()) {
    g_done = true;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_start < std::chrono::seconds(REXCVAR_GET(bo2zm_autoexec_delay_s))) {
    return;
  }
  g_done = true;
  REXLOG_INFO("[autoexec] running: {}", commands);
  bo2zm::autoexec::ExecuteCommands(ctx, base, commands);
}
