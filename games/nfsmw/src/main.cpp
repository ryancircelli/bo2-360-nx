// nfsmw - ReXGlue Recompiled Project
//
// Need for Speed: Most Wanted (Xbox 360, USA), recompiled. The title's facts (name, title id, executable, disc
// folder, setting defaults) are in games/nfsmw/game.toml; the shared GameApp
// runs whatever RegisterGame() adds (no hooks yet).

#include "generated/default/nfsmw_init.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/nfsmw/game.toml

REXCVAR_DEFINE_BOOL(nfsmw_stream_end_probe, false, "NFSMW",
                    "Diagnostics (PAS-18): log each time the audio stream voice ends a buffer before the stream "
                    "thread has marked it submitted");
REXCVAR_DEFINE_BOOL(nfsmw_stream_end_fix, true, "NFSMW",
                    "PAS-18: when the stream voice ends a buffer before the stream thread has marked it submitted, "
                    "hold the buffer-end until the mark is stored (else the race load can stall forever)");

REXCVAR_DEFINE_INT32(nfsmw_stream_race_stress_ms, 0, "NFSMW",
                     "Test only (PAS-18): after every 16th stream buffer submit, sleep this many ms before the "
                     "stream thread marks the slot submitted, to force the buffer-end race (0 = off)");

namespace {
// PAS-18. The game's audio stream thread (sub_825E3C30) submits a slot's 256-frame buffer to its voice
// (sub_82851D10) and only after that call returns stores the slot state 2 ("submitted"). The voice's buffer-end
// callback 0x825E4160, run by the audio render (one 5.33 ms pass per buffer), stores state 0 ("free") and signals
// the stream thread. If the stream thread is descheduled between the submit and its store for a render pass, the
// callback stores 0 first and the late store leaves the slot at 2 for good. The stream thread then never frees
// a slot again, never runs the resource manager's deferred-command queue (it does so only when its current slot
// is free), and the race loader waits forever on an unload queued there. On the console the stream thread is
// time-critical on its own hardware thread, so the window is a few instructions; on a busy host it is a whole
// scheduling slice.
//
// Both callbacks are reached by pointer, so the hooks below replace their dispatch entries. The fix records an
// early end (slot not yet at state 2) instead of applying it, and applies it from the next render pass (callback
// 0x825E4140, every pass) once the stream thread has stored 2: the order the console's timing guarantees.
std::atomic<uint64_t> g_end_calls{0}, g_end_early{0}, g_end_deferred_applied{0};
// Guest addresses of slot state words whose buffer ended early. The callback's r3 points at a temporary on the
// audio render thread's stack ({stream object, &slot state}), so only the state word's address is kept.
std::atomic<uint32_t> g_pending_state[2] = {0, 0};
auto g_probe_start = std::chrono::steady_clock::now();

uint32_t LoadBE32(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + addr));
}
int64_t ProbeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_probe_start)
      .count();
}

void StreamEndHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t state_ptr = LoadBE32(base, ctx.r3.u32 + 4);
  const uint32_t state = state_ptr ? LoadBE32(base, state_ptr) : 0xFFFFFFFFu;
  const uint64_t n = ++g_end_calls;
  if (state != 2) {
    const uint64_t k = ++g_end_early;
    bool held = false;
    if (REXCVAR_GET(nfsmw_stream_end_fix) && state == 1) {
      for (auto& slot : g_pending_state) {
        uint32_t expected = 0;
        if (slot.compare_exchange_strong(expected, state_ptr)) {
          held = true;
          break;
        }
      }
    }
    if (REXCVAR_GET(nfsmw_stream_end_probe) && k <= 200) {
      REXLOG_WARN("[stream-end] buffer-end #{} at {} ms: slot state word {:08X} = {} (expected 2); early ends {}{}",
                  n, ProbeMs(), state_ptr, state, k, held ? "; held until the stream thread stores 2" : "");
    }
    if (held) return;
  }
  sub_825E4160(ctx, base);
}

// Test only: the stream voice's SubmitSourceBuffer entry (inner voice vtable +28, reached by pointer from
// sub_82851D10). Sleeping after it widens the window before the stream thread stores state 2.
std::atomic<uint64_t> g_submits{0};
void StreamSubmitHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t desc = ctx.r4.u32;
  sub_82868F38(ctx, base);
  const int32_t ms = REXCVAR_GET(nfsmw_stream_race_stress_ms);
  if (ms <= 0 || !desc) return;
  const uint32_t state_ptr = LoadBE32(base, desc + 84);
  if (!state_ptr || LoadBE32(base, state_ptr) != 1) return;  // not a stream slot about to be marked 2
  if (++g_submits % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Every render pass: apply held buffer-ends whose slot the stream thread has now marked 2 (store 0, as 0x825E4160
// does), then run the pass callback, which signals the stream thread's event.
void StreamPassHook(PPCContext& ctx, uint8_t* base) {
  for (auto& slot : g_pending_state) {
    const uint32_t state_ptr = slot.load();
    if (!state_ptr) continue;
    auto* word = reinterpret_cast<std::atomic<uint32_t>*>(base + state_ptr);
    uint32_t expected = __builtin_bswap32(2u);
    if (!word->compare_exchange_strong(expected, 0u)) continue;  // the stream thread has not stored 2 yet
    slot.store(0);
    const uint64_t a = ++g_end_deferred_applied;
    if (REXCVAR_GET(nfsmw_stream_end_probe) && a <= 200) {
      REXLOG_WARN("[stream-end] held buffer-end applied at {} ms (slot state word {:08X}); applied {}", ProbeMs(),
                  state_ptr, a);
    }
  }
  sub_825E4140(ctx, base);
}
}  // namespace

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/nfsmw/game.toml");
  api.OnPostSetup([](rex::GameApp& app) {
    if (!REXCVAR_GET(nfsmw_stream_end_probe) && !REXCVAR_GET(nfsmw_stream_end_fix)) return;
    auto* rt = app.runtime();
    auto* fd = rt ? rt->function_dispatcher() : nullptr;
    const bool ok = fd && fd->SetFunction(0x825E4160, StreamEndHook) && fd->SetFunction(0x825E4140, StreamPassHook) &&
                    (REXCVAR_GET(nfsmw_stream_race_stress_ms) <= 0 || fd->SetFunction(0x82868F38, StreamSubmitHook));
    REXLOG_INFO("[stream-end] hooks {} (probe {}, fix {}, stress {} ms)", ok ? "installed" : "NOT installed",
                REXCVAR_GET(nfsmw_stream_end_probe), REXCVAR_GET(nfsmw_stream_end_fix),
                REXCVAR_GET(nfsmw_stream_race_stress_ms));
  });
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "nfsmw", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(nfsmw, CreateApp)
