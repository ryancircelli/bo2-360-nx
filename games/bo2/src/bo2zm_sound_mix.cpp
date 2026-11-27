// BO2's sound mixer runs its own mix jobs while it waits for them (split out of the removed bo2zm_waits.cpp; the
// spin-wait backoff in the SDK replaced the rest of that file).
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "bo2zm_game.h"

namespace {
uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

// --- The sound mixer's jobs ---------------------------------------------------------------------------------------
// BO2's sound mixer thread ("Sound Mix") hands each mix step to the job system and waits for it: five functions push
// one job on the preferred queue and then wait for its event with no timeout (sub_82428468). Their job types, by the
// names in their descriptors: sd_mix_voice (sub_82442B90), sd_mix_master (sub_8243F3C8), sd_mix_radverb
// (sub_8243F538), sd_voice_active (sub_8258A368), sd_voice_free (sub_8258A4C0). In a match the workers are busy with
// the frame at the guest priority, so every mix step waited behind them and the sound dropped out: on the Switch only
// ~700 blocks were mixed per 10 s instead of ~1,900, with the mixer itself already at the audio priority. So the
// waiting thread first runs the jobs of that type itself, with the game's own "wait for a job
// type" (sub_82493190, which Main uses from ~20 sites), at its own priority; then it waits for the event as before,
// by then set. A job a worker already took is simply waited for, as before.
REXCVAR_DEFINE_BOOL(bo2zm_sound_without_queue, true, "BO2",
                    "The sound mixer runs its own mix jobs (sd_mix_voice, sd_mix_master, ...) while it waits for "
                    "them, instead of waiting for a job worker to take them. false = the game's own wait");

REX_EXTERN(__imp__sub_82442B90);
REX_EXTERN(__imp__sub_8243F3C8);
REX_EXTERN(__imp__sub_8243F538);
REX_EXTERN(__imp__sub_8258A368);
REX_EXTERN(__imp__sub_8258A4C0);
REX_EXTERN(__imp__sub_82428468);
REX_EXTERN(__imp__sub_82493190);

namespace {

// Inside one of the five mix steps: the job definition it pushes (0 elsewhere). The definitions come from the code
// of each step (the lis/addi before its push).
thread_local uint32_t t_step_sound = 0;

// Every 10 s, from the thread that ran them: how many mix steps ran on the waiting thread, and their time.
std::atomic<uint32_t> g_steps_sound{0};
std::atomic<uint64_t> g_ns_sound{0};
std::atomic<uint64_t> g_max_ns_sound{0};
std::atomic<uint64_t> g_next_summary_sound{0};

template <typename F>
void SoundStep(PPCContext& ctx, uint8_t* base, uint32_t definition, F original) {
  const uint32_t earlier = t_step_sound;
  t_step_sound = REXCVAR_GET(bo2zm_sound_without_queue) ? definition : 0;
  original(ctx, base);
  t_step_sound = earlier;
}

void SummarizeSound(uint64_t now) {
  uint64_t next = g_next_summary_sound.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_summary_sound.compare_exchange_strong(next, now + 10'000'000'000ull);
    return;
  }
  if (now < next ||
      !g_next_summary_sound.compare_exchange_strong(next, now + 10'000'000'000ull)) {
    return;
  }
  const uint32_t steps = g_steps_sound.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_ns_sound.exchange(0, std::memory_order_relaxed);
  const uint64_t max_ns = g_max_ns_sound.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[sound] 10 s: {} mix steps run by the mixer itself, {:.1f} ms in total, longest {:.2f} ms", steps,
              double(ns) / 1e6, double(max_ns) / 1e6);
}

}  // namespace

REX_HOOK_RAW(sub_82442B90) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982C70, 0x82746E70), __imp__sub_82442B90); }  // sd_mix_voice
REX_HOOK_RAW(sub_8243F3C8) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982B70, 0x82746D70), __imp__sub_8243F3C8); }  // sd_mix_master
REX_HOOK_RAW(sub_8243F538) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982BF0, 0x82746DF0), __imp__sub_8243F538); }  // sd_mix_radverb
REX_HOOK_RAW(sub_8258A368) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82A7D1D0, 0x82848F50), __imp__sub_8258A368); }  // sd_voice_active
REX_HOOK_RAW(sub_8258A4C0) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82A7D1F0, 0x82848F70), __imp__sub_8258A4C0); }  // sd_voice_free

// WaitForSingleObject(*r3, INFINITE). In a mix step, run the step's jobs first (r3 and lr kept for the wait).
REX_HOOK_RAW(sub_82428468) {
  if (const uint32_t definition = t_step_sound) [[unlikely]] {
    t_step_sound = 0;  // once; a mix step inside the jobs it runs sets its own
    const uint64_t event = ctx.r3.u64;
    const uint64_t lr = ctx.lr;
    const uint64_t t0 = NowNs();
    ctx.r3.u64 = definition;
    __imp__sub_82493190(ctx, base);
    ctx.r3.u64 = event;
    ctx.lr = lr;
    const uint64_t now = NowNs();
    const uint64_t ns = now - t0;
    g_steps_sound.fetch_add(1, std::memory_order_relaxed);
    g_ns_sound.fetch_add(ns, std::memory_order_relaxed);
    uint64_t max_ns = g_max_ns_sound.load(std::memory_order_relaxed);
    while (ns > max_ns && !g_max_ns_sound.compare_exchange_weak(max_ns, ns, std::memory_order_relaxed)) {
    }
    SummarizeSound(now);
  }
  __imp__sub_82428468(ctx, base);
}

