// The sound system's wait for a zone's sound banks, with a shorter sleep.
//
// A zone with sound banks is not considered loaded until the sound system has taken them in: the thread loading it
// (the database thread, or the main thread at start) waits in the function hooked below. While the sound system is up
// (the word at 0x84116E04) and its banks-done byte (0x841575EC, which the sound update sets once the banks it was given
// are in) is 0, the game sleeps 30 ms and then pumps the sound system, which takes its lock and runs its queued work.
// The work only moves on at each pump, so a zone takes about as long on the PC as on the console. The DLC maps' load
// zones, which the game loads on the main thread the first time SOLO opens the map globe, froze the screen for one
// frame each: 0.3-0.6 s on the PC and 0.4-0.7 s on an OLED. A PC profile put about half of the main thread's samples
// during those freezes in that synchronous load, and 60 % of the database thread's in this sleep. At start-up the main
// thread also waited 1.1 s here, and Town's banks took 97 pumps (probably about 3 s at 30 ms each). This hook runs the
// same loop with the sleep set by bo2zm_sound_wait_ms.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "bo2zm_guest.h"

REXCVAR_DEFINE_INT32(bo2zm_sound_wait_ms, 1, "BO2",
                     "Milliseconds a thread loading a zone sleeps between two pumps of the sound system while it "
                     "waits for the zone's sound banks. The game sleeps 30 (the original: the five DLC maps' load "
                     "zones freeze the map globe 0.4-0.7 s each the first time SOLO opens it); 1 = as soon as they are "
                     "in")
    .range(1, 30);

REX_EXTERN(__imp__sub_82585F48);

namespace {

constexpr uint32_t kSoundUp = 0x8257F470;  // call only: the sound system is up (r3)
constexpr uint32_t kSoundPump = 0x82591320;  // call only: one pump of the sound system
constexpr uint32_t kSleep = 0x824E23C8;  // call only: Sleep(r3 ms)

constexpr uint32_t kBanksDone = 0x84116E00 + 0x407EC;  // byte: the banks the sound system was given are in
constexpr uint32_t kClearedBeforePump = 0x84173C80 + 0x508BE;  // byte the game's wait clears before every pump

std::atomic<uint32_t> g_waits_logged{0};

}  // namespace

REX_HOOK_RAW(sub_82585F48) {
  const int32_t ms = REXCVAR_GET(bo2zm_sound_wait_ms);
  const auto start = std::chrono::steady_clock::now();
  uint32_t pumps = 0;
  if (ms >= 30) {
    __imp__sub_82585F48(ctx, base);  // the game's own
  } else {
    const auto saved = ctx;
    ctx.r1.u64 = (ctx.r1.u32 - 0x70) & ~uint64_t(0xF);  // a frame of its own, as the game's function makes
    base[kClearedBeforePump] = 0;
    uint64_t up = 0;  // returned in r3, as the game's function leaves it
    for (;;) {
      bo2zm::guest::Call<kSoundUp>(ctx, base);
      up = ctx.r3.u64;
      if (ctx.r3.u32 == 0 || base[kBanksDone] != 0) {
        break;
      }
      ctx.r3.u64 = uint64_t(ms);
      bo2zm::guest::Call<kSleep>(ctx, base);
      base[kClearedBeforePump] = 0;
      bo2zm::guest::Call<kSoundPump>(ctx, base);
      ++pumps;
    }
    ctx = saved;
    ctx.r3.u64 = up;
  }
  const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (waited >= 20.0 && g_waits_logged.fetch_add(1, std::memory_order_relaxed) < 40) {
    REXLOG_INFO("[sound] waited {:.0f} ms for a zone's sound banks ({} pumps, {} ms apart{})", waited, pumps,
                std::min(ms, 30), ms >= 30 ? ", the game's own loop" : "");
  }
}
