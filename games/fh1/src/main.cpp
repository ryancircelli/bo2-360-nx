// fh1 - ReXGlue Recompiled Project
//
// Forza Horizon (Xbox 360, USA), recompiled. The title's facts (name, title id, executable, disc
// folder, setting defaults) are in games/fh1/game.toml; the shared GameApp
// runs whatever RegisterGame() adds, plus the IO completion override below.

#include "generated/default/fh1_init.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <rex/cvar.h>
#include <rex/game_api.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xevent.h>
#include <rex/system/xobject.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/fh1/game.toml

REXCVAR_DEFINE_BOOL(fh1_io_complete_probe, false, "FH1",
                    "Diagnostics: log IO completions whose event handle no longer names an event, with the "
                    "overlapped block's words, instead of only reaching the game's fatal handler");
REXCVAR_DEFINE_BOOL(fh1_io_complete_fix, true, "FH1",
                    "IO completion race: take the request's event object before publishing the completion status "
                    "and set that object, as the console kernel does, so a requester that sees the status and closes "
                    "or reuses its event first cannot send the IO worker into the game's fatal handler");
REXCVAR_DEFINE_INT32(fh1_io_complete_race_stress_ms, 0, "FH1",
                     "Test only: on the game's IO worker, sleep this many ms after storing a request's completion "
                     "status and before reading its event handle, to force the completion race (0 = off)");

namespace {
uint32_t Load32(uint8_t* base, uint32_t addr) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + addr)); }
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  *reinterpret_cast<uint32_t*>(base + addr) = __builtin_bswap32(v);
}

std::atomic<uint64_t> g_completions{0}, g_bad_events{0};

bool EventHandleLive(uint32_t handle) {
  auto* objects = REX_KERNEL_OBJECTS();
  if (!objects) return false;
  auto obj = objects->LookupObject<rex::system::XObject>(handle);
  return obj && obj->type() == rex::system::XObject::Type::Event;
}
}  // namespace

// The game's IO worker (thread loop sub_82C07068) pops 16-byte requests {file, buffer, length, overlapped} and
// runs this per request: perform the transfer (sub_82C05E70), store the overlapped block's status (+0) and byte
// count (+4), then read its event handle (+16) and set it (sub_8247AF78 = NtSetEvent; failure goes to the game's
// fatal handler). This is a straight C++ copy of the recompiled function so the step between the status store and
// the event-handle read can be widened (stress) and observed (probe); behaviour is otherwise identical.
extern "C" void sub_82C06730(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t req = ctx.r3.u32;
  const uint32_t sp = ctx.r1.u32;
  const uint32_t frame = sp - 112;
  Store32(base, sp - 8, static_cast<uint32_t>(ctx.lr));
  Store32(base, frame, sp);
  ctx.r1.u32 = frame;
  const uint32_t out = frame + 80;
  ctx.r5.u64 = Load32(base, req + 8);
  ctx.r6.u64 = out;
  ctx.r4.u64 = Load32(base, req + 4);
  ctx.r3.u64 = Load32(base, req + 0);
  ctx.r7.u64 = Load32(base, req + 12);
  ctx.lr = 0x82C06760;
  sub_82C05E70(ctx, base);
  const uint32_t transferred = Load32(base, out);
  ctx.r1.u32 = sp;
  ctx.lr = Load32(base, sp - 8);

  uint32_t ov = Load32(base, req + 12);
  if (REXCVAR_GET(fh1_io_complete_fix)) {
    // Until the status store below the requester cannot treat the request as complete, so the overlapped block
    // and its event are still its live ones here. Hold the event object across the store.
    const uint32_t early_ev = Load32(base, ov + 16);
    rex::system::object_ref<rex::system::XEvent> event;
    if (early_ev != 0 && early_ev != 0xFFFFFFFFu) {
      if (auto* objects = REX_KERNEL_OBJECTS()) event = objects->LookupObject<rex::system::XEvent>(early_ev);
    }
    if (event) {
      Store32(base, ov + 0, 0);
      ov = Load32(base, req + 12);
      Store32(base, ov + 4, transferred);
      ++g_completions;
      const int32_t stress_ms = REXCVAR_GET(fh1_io_complete_race_stress_ms);
      if (stress_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stress_ms));
      event->Set(0, false);
      ctx.r3.u64 = 1;
      return;
    }
    // No event (none requested, or not a live event before completion): the game's own path below.
  }
  Store32(base, ov + 0, 0);
  ov = Load32(base, req + 12);
  Store32(base, ov + 4, transferred);
  ++g_completions;

  const int32_t stress_ms = REXCVAR_GET(fh1_io_complete_race_stress_ms);
  if (stress_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stress_ms));

  ov = Load32(base, req + 12);
  const uint32_t ev = Load32(base, ov + 16);
  if (ev == 0 || ev == 0xFFFFFFFFu) return;
  ctx.r3.u64 = ev;
  ctx.lr = 0x82C0679C;
  sub_8247AF78(ctx, base);
  ctx.lr = Load32(base, sp - 8);
  if (ctx.r3.u32 != 0) return;

  const uint64_t bad = ++g_bad_events;
  if (REXCVAR_GET(fh1_io_complete_probe) && bad <= 50) {
    REXLOG_ERROR(
        "[io-complete] NtSetEvent failed: overlapped {:08X} event {:08X} (live event now: {}); words {:08X} {:08X} "
        "{:08X} {:08X} {:08X}; request file {:08X} buffer {:08X} length {:08X}; transferred {:08X}; completion {}",
        ov, ev, EventHandleLive(ev), Load32(base, ov + 0), Load32(base, ov + 4), Load32(base, ov + 8),
        Load32(base, ov + 12), Load32(base, ov + 16), Load32(base, req + 0), Load32(base, req + 4),
        Load32(base, req + 8), transferred, g_completions.load());
  }

  // The game's fatal path at 0x82C067A4, unchanged.
  const uint32_t g = 0x83440000;
  ctx.r10.u64 = Load32(base, Load32(base, req + 0) + 32);
  ctx.r31.u64 = g;
  ctx.r11.u64 = 0x82140000 + 11488;
  ctx.r30.u64 = g - 26348;
  Store32(base, ctx.r30.u32 - 24, ctx.r11.u32);
  Store32(base, ctx.r30.u32 - 20, ctx.r10.u32);
  ctx.lr = 0x82C067C8;
  sub_82453190(ctx, base);
  Store32(base, ctx.r30.u32 - 16, ctx.r3.u32);
  ctx.r11.u64 = Load32(base, g - 26348);
  ctx.r3.u64 = Load32(base, ctx.r30.u32 + 4);
  ctx.lr = 0x82C067DC;
  REX_CALL_INDIRECT_FUNC(ctx.r11.u32);
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/fh1/game.toml");
  api.OnPostSetup([](rex::GameApp&) {
    REXLOG_INFO("[io-complete] override active (fix {}, probe {}, stress {} ms)", REXCVAR_GET(fh1_io_complete_fix),
                REXCVAR_GET(fh1_io_complete_probe), REXCVAR_GET(fh1_io_complete_race_stress_ms));
  });
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "fh1", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(fh1, CreateApp)
