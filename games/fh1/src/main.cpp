// fh1 - ReXGlue Recompiled Project
//
// Forza Horizon (Xbox 360, USA), recompiled. The title's facts (name, title id, executable, disc
// folder, setting defaults) are in games/fh1/game.toml; the shared GameApp
// runs whatever RegisterGame() adds, plus the IO completion override below.

#include "generated/default/fh1_init.h"

#include <atomic>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unordered_map>
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
REXCVAR_DEFINE_INT32(fh1_io_complete_race_stress_at, 0, "FH1",
                     "Test only: where the stress sleep goes: 0 = after the status store, 1 = before the transfer "
                     "(the IO worker lagging behind its requesters)");

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

// Probe bookkeeping: every request the game builds (sub_82BFFD08) with the guest call chain that submitted it,
// keyed by overlapped address, so a dead event at completion and an event closed while its request is still queued
// can both be traced to their requester and closer.
struct Pending {
  uint32_t event = 0;
  uint32_t thread = 0;
  uint32_t chain[6] = {};
  uint64_t serial = 0;
};
std::mutex g_pending_mutex;
std::unordered_map<uint32_t, Pending> g_pending;       // overlapped -> request
std::unordered_map<uint32_t, uint32_t> g_pending_events;  // event handle -> requests queued with it
std::atomic<uint64_t> g_submits{0}, g_probe_logs{0};

uint32_t HostThreadTag() { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()) >> 4); }

void GuestChain(uint8_t* base, uint32_t sp, uint32_t lr, uint32_t (&chain)[6]) {
  chain[0] = lr;
  uint32_t s = sp;
  for (int i = 1; i < 6; ++i) {
    if (s < 0x10000 || s >= 0xF0000000u) break;
    const uint32_t back = Load32(base, s);
    if (back <= s || back - s > 0x100000) break;
    chain[i] = Load32(base, back - 8);
    s = back;
  }
}

bool ProbeBudget() { return ++g_probe_logs <= 200; }

std::string ChainText(const uint32_t (&c)[6]) {
  return fmt::format("{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", c[0], c[1], c[2], c[3], c[4], c[5]);
}
void ProbeComplete(uint8_t* base, uint32_t ov) {
  (void)base;
  const uint64_t n = g_completions.load();
  {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    auto it = g_pending.find(ov);
    if (it != g_pending.end()) {
      if (--g_pending_events[it->second.event] == 0) g_pending_events.erase(it->second.event);
      g_pending.erase(it);
    }
  }
  if (n == 1 || n == 10 || n == 100 || n == 1000 || n % 10000 == 0)
    REXLOG_INFO("[io-complete] {} completions, {} submits, {} failed event sets", n, g_submits.load(),
                g_bad_events.load());
}
}  // namespace

// Request builder, called by the async submit (sub_82C067F8) with r3 = request, r7 = overlapped. Probe only: record
// the request and its submitter, and flag an overlapped submitted again while its last request is still queued.
extern "C" void __imp__sub_82BFFD08(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82BFFD08(PPCContext& __restrict ctx, uint8_t* base) {
  if (REXCVAR_GET(fh1_io_complete_probe)) {
    const uint32_t ov = ctx.r7.u32;
    Pending p;
    p.event = Load32(base, ov + 16);
    p.thread = HostThreadTag();
    p.serial = ++g_submits;
    GuestChain(base, ctx.r1.u32, static_cast<uint32_t>(ctx.lr), p.chain);
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    auto it = g_pending.find(ov);
    if (it != g_pending.end()) {
      if (ProbeBudget())
        REXLOG_ERROR("[io-complete] overlapped {:08X} resubmitted while queued: old event {:08X} thread {:08X} "
                     "submit #{} chain {}; new event {:08X} thread {:08X} submit #{} chain {}",
                     ov, it->second.event, it->second.thread, it->second.serial, ChainText(it->second.chain),
                     p.event, p.thread, p.serial, ChainText(p.chain));
      if (--g_pending_events[it->second.event] == 0) g_pending_events.erase(it->second.event);
    }
    g_pending[ov] = p;
    ++g_pending_events[p.event];
  }
  __imp__sub_82BFFD08(ctx, base);
}

// NtClose import, probe only: flag the close of an event that a queued IO request will still set.
using GuestFn = void (*)(PPCContext&, uint8_t*);
extern "C" void __imp__NtClose(PPCContext& __restrict ctx, uint8_t* base) {
  static const GuestFn real = reinterpret_cast<GuestFn>(dlsym(RTLD_NEXT, "__imp__NtClose"));
  if (REXCVAR_GET(fh1_io_complete_probe)) {
    const uint32_t h = ctx.r3.u32;
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    if (g_pending_events.count(h) && ProbeBudget()) {
      uint32_t closer[6] = {};
      GuestChain(base, ctx.r1.u32, static_cast<uint32_t>(ctx.lr), closer);
      for (auto& [ov, p] : g_pending) {
        if (p.event != h) continue;
        REXLOG_ERROR("[io-complete] event {:08X} closed while its request is queued: closer thread {:08X} chain {}; "
                     "request overlapped {:08X} status {:08X} thread {:08X} submit #{} chain {}",
                     h, HostThreadTag(), ChainText(closer), ov, Load32(base, ov + 0), p.thread, p.serial,
                     ChainText(p.chain));
      }
    }
  }
  real(ctx, base);
}

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
  const bool probe = REXCVAR_GET(fh1_io_complete_probe);
  const int32_t stress_ms = REXCVAR_GET(fh1_io_complete_race_stress_ms);
  const int32_t stress_at = REXCVAR_GET(fh1_io_complete_race_stress_at);
  if (stress_ms > 0 && stress_at == 1) std::this_thread::sleep_for(std::chrono::milliseconds(stress_ms));
  {
    const uint32_t ov0 = Load32(base, req + 12);
    const uint32_t ev0 = Load32(base, ov0 + 16);
    if (probe && ev0 != 0 && ev0 != 0xFFFFFFFFu && !EventHandleLive(ev0) && ProbeBudget()) {
      std::lock_guard<std::mutex> lock(g_pending_mutex);
      auto it = g_pending.find(ov0);
      REXLOG_ERROR("[io-complete] request reached the IO worker with a dead event: overlapped {:08X} event {:08X} "
                   "status {:08X}; file {:08X} buffer {:08X} length {:08X}; submitter {}",
                   ov0, ev0, Load32(base, ov0 + 0), Load32(base, req + 0), Load32(base, req + 4),
                   Load32(base, req + 8),
                   it == g_pending.end() ? std::string("unknown")
                                         : fmt::format("event {:08X} thread {:08X} submit #{} chain {}",
                                                       it->second.event, it->second.thread, it->second.serial,
                                                       ChainText(it->second.chain)));
    }
  }
  ctx.r5.u64 = Load32(base, req + 8);
  ctx.r6.u64 = out;
  ctx.r4.u64 = Load32(base, req + 4);
  ctx.r3.u64 = Load32(base, req + 0);
  ctx.r7.u64 = Load32(base, req + 12);
  uint32_t ov_before[5] = {};
  const uint32_t ov_in = ctx.r7.u32;
  if (probe && ov_in) {
    for (int i = 0; i < 5; ++i) ov_before[i] = Load32(base, ov_in + 4 * i);
    const uint32_t buf = ctx.r4.u32, len = ctx.r5.u32;
    if (buf < ov_in + 20 && ov_in < buf + len && ProbeBudget())
      REXLOG_ERROR("[io-complete] transfer buffer {:08X}+{:X} covers its own overlapped {:08X}; file {:08X}", buf,
                   len, ov_in, ctx.r3.u32);
  }
  ctx.lr = 0x82C06760;
  sub_82C05E70(ctx, base);
  if (probe && ov_in) {
    if (Load32(base, ov_in + 16) != ov_before[4] || Load32(base, ov_in + 8) != ov_before[2] ||
        Load32(base, ov_in + 12) != ov_before[3] || Load32(base, req + 12) != ov_in) {
      if (ProbeBudget())
        REXLOG_ERROR("[io-complete] overlapped {:08X} changed during the transfer: before {:08X} {:08X} {:08X} "
                     "{:08X} {:08X}, after {:08X} {:08X} {:08X} {:08X} {:08X}; request now {:08X}; file {:08X} "
                     "buffer {:08X} length {:08X}",
                     ov_in, ov_before[0], ov_before[1], ov_before[2], ov_before[3], ov_before[4],
                     Load32(base, ov_in + 0), Load32(base, ov_in + 4), Load32(base, ov_in + 8),
                     Load32(base, ov_in + 12), Load32(base, ov_in + 16), Load32(base, req + 12),
                     Load32(base, req + 0), Load32(base, req + 4), Load32(base, req + 8));
    }
  }
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
      if (stress_ms > 0 && stress_at == 0) std::this_thread::sleep_for(std::chrono::milliseconds(stress_ms));
      if (probe) ProbeComplete(base, ov);
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
  if (stress_ms > 0 && stress_at == 0) std::this_thread::sleep_for(std::chrono::milliseconds(stress_ms));

  ov = Load32(base, req + 12);
  const uint32_t ev = Load32(base, ov + 16);
  if (ev == 0 || ev == 0xFFFFFFFFu) {
    if (probe) ProbeComplete(base, ov);
    return;
  }
  ctx.r3.u64 = ev;
  ctx.lr = 0x82C0679C;
  sub_8247AF78(ctx, base);
  ctx.lr = Load32(base, sp - 8);
  if (probe) ProbeComplete(base, ov);
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
    REXLOG_INFO("[io-complete] override active (fix {}, probe {}, stress {} ms at {})",
                REXCVAR_GET(fh1_io_complete_fix), REXCVAR_GET(fh1_io_complete_probe),
                REXCVAR_GET(fh1_io_complete_race_stress_ms), REXCVAR_GET(fh1_io_complete_race_stress_at));
  });
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "fh1", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(fh1, CreateApp)
