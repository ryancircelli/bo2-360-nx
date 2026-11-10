// Switch-only system setup done before the game starts.
//
// Sockets: the runtime implements the game's NetDll_socket/bind with BSD sockets, which on Horizon need libnx's bsd
// service started first. Nothing in the runtime starts it, so on the first hardware run every socket() failed and BO2
// stopped at init with a fatal "WARNING: Couldn't allocate IP port" (Com_Error type 0, fmt 0x820610D0). BO2 opens a
// UDP port even for offline play.

#include "bo2zm_switch.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/thread.h>
#include <rex/thread_audio.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
extern "C" char** __system_argv;
#include "core/threading_switch.h"  // SDK src/ is on the include path (CMakeLists.txt)
extern "C" void RexSwitchPerfSetStackSampling(int forced);
#include "bo2zm_switch_mutex.h"
#endif

#ifndef BO2ZM_BUILD_ID
#define BO2ZM_BUILD_ID "dev"
#endif

REXCVAR_DEFINE_BOOL(bo2zm_xaudio_threads_priority, true, "BO2",
                    "The game's two audio engine threads run at the audio priority: 0x2B instead of the guest 0x3B on "
                    "the Switch, the SDK's audio_thread_policy elsewhere. false = like the other guest threads");

REXCVAR_DEFINE_INT32(bo2zm_profile_stacks, -1, "BO2",
                     "Switch: stack sampling in rex_profile.log (pauses every thread each millisecond, which costs "
                     "frames): 1 = on, 0 = off, -1 = on when logs/rex/profile_stacks.flag exists. Test tomls set it "
                     "per run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Off: no gain measured. In a match the A/B gave -0.7 % against -1.2 % for a control factor that changes nothing, and
// on a map's first visit the heap lock still set the compile threads' pace.
REXCVAR_DEFINE_BOOL(bo2zm_malloc_spin, false, "BO2",
                    "Switch: the C heap's lock (every malloc and free) spins a few microseconds before it sleeps in "
                    "the kernel, as the host mutexes do (bo2zm_switch_mutex.cpp). false = it sleeps at once, as "
                    "newlib's own lock did")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_malloc_spin_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_malloc_spin. 0 = not "
                     "part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The audio threads (0x2B) preempt every other thread, the renderer's ring thread (0x2D) included. In a profile on
 * the Switch at stock clocks the mixer, the XMA decoder and the audio engine threads took ~0.85 of a core, and the
 * ring, the thread that sets the frame rate (busy for the whole frame, ~64 ms of work per 64 ms frame), lost about
 * 3 % of its samples to preemption on top of that. The ring prefers core 1 (bo2zm_native_ring_core); with this the
 * audio threads may only run on the other two cores, where they preempt game threads at 0x3B instead, which wait for
 * the ring anyway. This is not an exclusive pin of the ring (that was measured and made the dips worse): the ring
 * keeps its three cores, and only audio loses one.
 */
REXCVAR_DEFINE_BOOL(bo2zm_audio_off_ring_core, true, "BO2",
                    "Switch: the audio threads run on every core but the ring thread's (bo2zm_native_ring_core). "
                    "Sets the SDK's switch_audio_core_mask unless the toml gives it");

// Core 3, experimental (the SDK's switch_core3.cpp, docs/research/08-core3.md). Only does anything when the game is
// started from a forwarder that allows cores 0-3 (sphaira: CPU Cores 4) and bo2zm_cpu_cores is 4 (Extras >
// EXPERIMENTAL > CPU CORES, saved; 3 at first); with hbmenu or a normal forwarder core 3 is not the process's.
REXCVAR_DEFINE_INT32(bo2zm_cpu_cores, 3, "BO2",
                     "Switch, experimental: 4 = the game also uses the fourth CPU core (the threads bo2zm_core3_threads "
                     "names), which only a forwarder made with four cores allows (sphaira: CPU Cores 4); started any "
                     "other way, 4 changes nothing. 3 = the three cores every launch has. Extras > EXPERIMENTAL > CPU "
                     "CORES sets it at once and saves the pick (cpu_cores); a value set here wins over the saved pick")
    .range(3, 4);
// The default, as measured on a Switch Lite (Town on very low from round 20, 180 s routes). The mixer and the XMA
// decoder move to core 3 at their own audio priority (0x2B = 43): with the Server alone on core 3 as well, that gave
// 52.4 FPS against 43.4 with no thread moved. The audio threads keep their priority because at 63 the XMA decoder
// answered a kick in ~4.8 ms on average and up to ~21 ms (on three cores: 0.3 ms, at most ~5 ms), and the game's own
// voices, the zombies' first, audibly stuttered although the mixer never ran dry (all three threads at 63 gave
// 53.5-53.8 FPS). At 0x2B the decoder answers in 1.7 ms on average, at most 4.8 ms.
// The Server, though, is not left alone on core 3: there it got only ~30-35 % of a core at round 20 (core 3 also runs
// the system, ~37-40 %, and the audio threads, ~28 %) where it needs ~46 % (on three cores), so the game's simulation
// fell behind and the zombies moved in jerks while the frame rate stayed high. On all four cores at the game's own
// priority it takes core 3's spare time on top of its share of cores 0-2, which only it may use: ~50 % of a core, and
// 52.1 FPS against 44.6 with CPU CORES 3.
REXCVAR_DEFINE_STRING(bo2zm_core3_threads, "Server:0xF:59,Sound Mix:0x8:43,XMA Decoder:0x8:43", "BO2",
                      "Switch, experimental, 4-core forwarder only: threads moved to core 3, as comma-separated "
                      "\"name prefix[:core mask[:priority]]\" (mask 0x8 = core 3 alone, the default; 0xF = cores "
                      "0-3; priority kept when left out, 63 = time-sliced with the system's background threads). "
                      "Threads alone on core 3 at different priorities: only the audio band (43 and below) may be "
                      "above the others, else all get 63. \"\" = none moved")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_core3_failsafe_percent, 85, "BO2",
                     "Switch, experimental: the four cores switch themselves off when our threads pinned to core 3 hold "
                     "more than this share of it (%) for bo2zm_core3_failsafe_s seconds in a row, so the system's "
                     "services there can run (a whole-console freeze otherwise; switch_core3.cpp). In play they take "
                     "~30 %. Test tomls lower it to see it act")
    .range(1, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_core3_failsafe_s, 2, "BO2",
                     "Switch, experimental: seconds in a row over bo2zm_core3_failsafe_percent before the failsafe acts")
    .range(1, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_core3_clamp, true, "BO2",
                    "Switch, experimental: every thread a core-3 rule does not name stays on cores 0-2, as with a "
                    "normal launch. false = libnx's default with a 4-core forwarder: every thread may use core 3")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_core3_meter, true, "BO2",
                    "Switch, experimental, 4-core forwarder only: a thread on core 3 measures its load every second "
                    "([core3] lines, rex_profile.log), split into our threads and the system")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Mesa's disk cache keeps a second copy of what cache/bo2zm_native_pipelines.bin holds, and creating the pipeline
// cache from that file hands Mesa every one of its objects again at each start (vk_pipeline_cache_load puts them all):
// its four queue threads copy and compress ~44 MB and then find each entry already in the file, which costs ~20 % of a
// core through the loading (measured on an OLED).
REXCVAR_DEFINE_BOOL(bo2zm_mesa_shader_cache, true, "BO2",
                    "Switch: Mesa keeps its own shader cache on the SD (sdmc:/.mesa). false = off for this process "
                    "(MESA_SHADER_CACHE_DISABLE): the game's own pipeline file still holds every pipeline the renderer "
                    "made; the few made without it are compiled at each start. For an A/B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)
extern "C" void RexSwitchCore3SetLog(void (*log)(const char*));
extern "C" bool RexSwitchCore3Configure(const char* rules, bool clamp, bool meter, bool active);
extern "C" bool RexSwitchCore3SetActive(bool on);
extern "C" bool RexSwitchCore3Active(void);
extern "C" bool RexSwitchCore3Tripped(void);
extern "C" void RexSwitchCore3SetHogLimit(int percent, int seconds);
#endif

namespace bo2zm {

// Which executable is running: size and modification time of the file, so a
// log always says which build produced it (the SD card may hold an old NRO).
static void LogBuildIdentity() {
  std::error_code ec;
#if defined(__SWITCH__)
  const std::filesystem::path exe = (__system_argv && __system_argv[0]) ? __system_argv[0] : "";
#else
  const std::filesystem::path exe = rex::filesystem::GetExecutablePath();
#endif
  const auto size = std::filesystem::file_size(exe, ec);
  const auto mtime = std::filesystem::last_write_time(exe, ec);
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
  REXLOG_INFO("[bo2zm] build {} | executable {} size {} mtime {}", BO2ZM_BUILD_ID, exe.string(), size,
              secs);
}

const char* BuildId() { return BO2ZM_BUILD_ID; }

int CpuCoresSetting() {
  return REXCVAR_GET(bo2zm_cpu_cores);
}

void SetCpuCoresSetting(int cores) {
  rex::cvar::SetFlagByName("bo2zm_cpu_cores", cores == 4 ? "4" : "3");
#if defined(__SWITCH__)
  RexSwitchCore3SetActive(cores == 4);
#endif
}

bool CoresSwitchedOff() {
#if defined(__SWITCH__)
  return RexSwitchCore3Tripped();
#else
  return false;
#endif
}

int CoresInUse() {
#if defined(__SWITCH__)
  return RexSwitchCore3Active() ? 4 : 3;
#else
  return 0;
#endif
}

#if defined(__SWITCH__)
// The "Default" display as libnx's default window opened it (viOpenDisplay is wrapped: app/CMakeLists.txt). A second
// open of the same display fails with vi 0x1272 (with VSYNC on, that used to keep the refresh pacing from ever
// running), but the vsync event only needs the display's id.
static std::atomic<uint64_t> g_default_display_id{~uint64_t(0)};
extern "C" Result __real_viOpenDisplay(const char* display_name, ViDisplay* display);
extern "C" Result __wrap_viOpenDisplay(const char* display_name, ViDisplay* display) {
  const Result rc = __real_viOpenDisplay(display_name, display);
  if (R_SUCCEEDED(rc) && display && display_name && std::strcmp(display_name, "Default") == 0) {
    g_default_display_id.store(display->display_id, std::memory_order_relaxed);
  }
  return rc;
}

// The default display's vsync event. libnx's default window opens the display but never asks for this event, so it is
// ours to take; the display stays open with it.
static Event* RefreshEvent() {
  static Event event{};
  static const bool taken = [] {
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    const uint64_t id = g_default_display_id.load(std::memory_order_relaxed);
    if (id != ~uint64_t(0)) {
      ViDisplay display{};
      display.display_id = id;
      display.initialized = true;
      rc = viGetDisplayVsyncEvent(&display, &event);
    }
    if (R_FAILED(rc)) {
      static ViDisplay display{};
      rc = viOpenDefaultDisplay(&display);
      if (R_SUCCEEDED(rc)) {
        rc = viGetDisplayVsyncEvent(&display, &event);
      }
    }
    if (R_FAILED(rc)) {
      REXLOG_WARN("[switch] no screen refresh signal (vi 0x{:X}, default display id {}): VSYNC leaves the pacing to "
                  "the screen",
                  rc, id == ~uint64_t(0) ? std::string("not seen") : std::to_string(id));
      return false;
    }
    REXLOG_INFO("[switch] screen refresh signal taken (default display id {}): with VSYNC each frame in a match starts "
                "on a refresh",
                id);
    return true;
  }();
  return taken ? &event : nullptr;
}
#endif

bool HasRefreshSignal() {
#if defined(__SWITCH__)
  return RefreshEvent() != nullptr;
#else
  return false;
#endif
}

bool WaitForNextRefresh(int timeout_ms) {
#if defined(__SWITCH__)
  Event* event = RefreshEvent();
  if (!event) {
    return false;
  }
  eventClear(event);  // a refresh signalled before now does not count
  return R_SUCCEEDED(eventWait(event, uint64_t(timeout_ms) * 1000000));
#else
  (void)timeout_ms;
  return false;
#endif
}

int ProcessCores() {
#if defined(__SWITCH__)
  static const int cores = [] {
    u64 mask = 0;
    return R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ? __builtin_popcountll(mask) : 3;
  }();
  return cores;
#else
  return 0;
#endif
}

void InitSwitchServices() {
  LogBuildIdentity();
#if !defined(__SWITCH__)
  // The guest thread BO2 mixes its sound on ("Sound Mix", see the Switch block below) runs in the audio scheduling
  // class (the SDK's audio_thread_policy) with the Audio Worker it answers. A value from the toml wins.
  if (rex::cvar::GetFlagByName("audio_guest_threads").empty()) {
    rex::cvar::SetFlagByName("audio_guest_threads", "Sound Mix");
  }
#endif
#if defined(__SWITCH__)
  if (REXCVAR_GET(bo2zm_malloc_spin)) {
    SetMallocSpin(true);
    REXLOG_INFO("[switch] the C heap's lock spins before it sleeps (bo2zm_malloc_spin)");
  }
  if (const int32_t factor = REXCVAR_GET(bo2zm_malloc_spin_ab)) {
    SetMallocSpinFactor(factor);
    REXLOG_INFO("[switch] the C heap's lock spins in the A/B arms of factor {} (bo2zm_malloc_spin_ab)", factor);
  }
  if (const int32_t stacks = REXCVAR_GET(bo2zm_profile_stacks); stacks == 0 || stacks == 1) {
    RexSwitchPerfSetStackSampling(stacks);
    REXLOG_INFO("[switch] stack sampling {} (bo2zm_profile_stacks)", stacks ? "on" : "off");
  }
  // Before the game's threads start, so each one named in a rule is moved as it names itself.
  RexSwitchCore3SetLog([](const char* text) { REXLOG_INFO("{}", text); });
  RexSwitchCore3SetHogLimit(REXCVAR_GET(bo2zm_core3_failsafe_percent), REXCVAR_GET(bo2zm_core3_failsafe_s));
  RexSwitchCore3Configure(REXCVAR_GET(bo2zm_core3_threads).c_str(), REXCVAR_GET(bo2zm_core3_clamp),
                          REXCVAR_GET(bo2zm_core3_meter), REXCVAR_GET(bo2zm_cpu_cores) == 4);
  // A first visit measured on a console that has made the visit: Mesa keeps every shader it compiles on the SD
  // (sdmc:/.mesa, its disk cache, read when NVK sets up the GPU), so the app's own empty pipeline cache is not enough.
  // Set before the GPU is: nvk_physical_device_init_pipeline_cache reads it.
  // bo2zm_native_test_cold_pipelines too: a new driver would not find its shaders there either.
  if (rex::cvar::Query<bool>("bo2zm_native_test_fresh_pipelines") ||
      rex::cvar::Query<bool>("bo2zm_native_test_cold_pipelines")) {
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    REXLOG_INFO("[switch] Mesa's shader cache off (bo2zm_native_test_fresh_pipelines or _cold_pipelines): every "
                "shader is compiled");
  } else if (!REXCVAR_GET(bo2zm_mesa_shader_cache)) {
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    REXLOG_INFO("[switch] Mesa's shader cache off (bo2zm_mesa_shader_cache = false)");
  }
  // BO2 mixes its sound on the guest thread it names "Sound Mix": the audio callback BO2 registers runs on the SDK's
  // Audio Worker, sets an event, waits for the mix and submits it (no game addresses here, or tools/direct_calls.py
  // would take them for hooks). At the guest priority the mixer waited for its 10 ms turn behind the busy game
  // threads, and in a match the sound dropped out: only 680 blocks mixed, and 1352 requests without data, in 10 s. A
  // value from the toml wins.
  if (rex::cvar::GetFlagByName("switch_audio_guest_threads").empty()) {
    rex::cvar::SetFlagByName("switch_audio_guest_threads", "Sound Mix");
  }
  if (REXCVAR_GET(bo2zm_audio_off_ring_core) && rex::cvar::Query<int32_t>("switch_audio_core_mask") == 0) {
    const int32_t core = rex::cvar::Query<int32_t>("bo2zm_native_ring_core");
    if (core >= 0 && core <= 2) {
      const int mask = 0x7 & ~(1 << core);
      rex::cvar::SetFlagByName("switch_audio_core_mask", std::to_string(mask));
      REXLOG_INFO("[switch] audio threads on cores mask {:#x} (the ring thread prefers core {})", mask, core);
    }
  }
  const Result rc = socketInitializeDefault();
  if (R_SUCCEEDED(rc)) {
    REXLOG_INFO("[switch] sockets ready (bsd service)");
  } else {
    REXLOG_ERROR("[switch] socketInitializeDefault failed: {:#x}", rc);
  }
#endif
}

}  // namespace bo2zm

// --- The game's audio engine threads (Switch) ----------------------------------------------------------------------
// Each sound block goes through two threads of the audio engine linked into the game: the callback on the SDK's
// Audio Worker wakes the engine thread, which splits the block's processing with a second thread and only then
// answers the callback. Both are unnamed guest threads, so they ran at the guest priority (0x3B), and in a match each
// hand-off waited for a 10 ms turn: ~1,000 of ~1,900 blocks per 10 s were still late, with the mixer ("Sound Mix")
// already idle with its ring full. Their thread functions are hooked to
// raise the thread to the audio priority before the engine loop starts; both loops block between blocks.
REX_EXTERN(__imp__sub_8281A248);
REX_EXTERN(__imp__sub_8281A2A8);

namespace {
void AudioThread([[maybe_unused]] const char* which) {
#if defined(__SWITCH__)
  if (!REXCVAR_GET(bo2zm_xaudio_threads_priority)) {
    return;
  }
  const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
  if (R_SUCCEEDED(rc)) {
    REXLOG_INFO("[switch] audio engine {} thread: priority 0x2b", which);
  } else {
    REXLOG_WARN("[switch] audio engine {} thread: svcSetThreadPriority failed: {:#x}", which, rc);
  }
  // Like the SDK's audio threads (switch_audio_core_mask, set in InitSwitchServices).
  const int32_t value = rex::cvar::Query<int32_t>("switch_audio_core_mask");
  if (value > 0) {
    const bool accepted = RexSwitchSetCurrentThreadCoreMask(uint64_t(value));
    REXLOG_INFO("[switch] audio engine {} thread: core mask {:#x}{}", which, value,
                accepted ? "" : " REFUSED by the kernel");
  }
#else
  // Elsewhere the same two hand-offs per block waited behind the game threads on a busy machine: the SDK's audio
  // scheduling class (audio_thread_policy), as for the Audio Worker and "Sound Mix".
  if (!REXCVAR_GET(bo2zm_xaudio_threads_priority)) {
    REXLOG_INFO("[audio] audio engine {} thread started (bo2zm_xaudio_threads_priority = false)", which);
    return;
  }
  REXLOG_INFO("[audio] audio engine {} thread: {}", which, rex::thread::SetThreadAudioPriority());
#endif
}
}  // namespace

REX_HOOK_RAW(sub_8281A248) {  // the engine thread's function
  AudioThread("main");
  __imp__sub_8281A248(ctx, base);
}
REX_HOOK_RAW(sub_8281A2A8) {  // the second processing thread's function
  AudioThread("second");
  __imp__sub_8281A2A8(ctx, base);
}
