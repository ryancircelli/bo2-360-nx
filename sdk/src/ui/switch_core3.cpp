/*
 * Core 3 (experimental; Extras > EXPERIMENTAL > CPU CORES in the game, docs/research/08-core3.md).
 *
 * An application gets cores 0-2. Core 3 runs the system's services: the ones that serve the game's own file reads,
 * sockets, GPU driver calls and input, plus sysmodules and overlays. A forwarder whose NPDM allows cores 0-3
 * (sphaira's "4 CPU Cores": ThreadInfo highest core 3, lowest priority 63) lets the process use it. libnx then gives
 * every new pthread the whole process mask (newlib's __syscall_thread_create: svcSetThreadCoreMask(handle, -1, the
 * InfoType_CoreMask value)), so with such a forwarder every thread of the game would spread onto core 3, at
 * priorities the kernel never time-slices there against the system: it rotates priority 63 on core 3 and 59 on cores
 * 0-2 every 10 ms (KDpcManager, Atmosphère's kern_k_dpc_manager.cpp). Hence:
 *
 *  - Every svcSetThreadCoreMask (wrapped, rexglue_switch.cmake) loses core 3, unless its thread was placed there on
 *    purpose. The main thread, whose mask the forwarder's loader set to the whole process mask, is clamped in
 *    RexSwitchCore3Configure.
 *  - Threads whose name starts with a rule's prefix are kept when they name themselves (pthread_setname_np) or, for
 *    those already named, when the rules arrive, with the mask and priority the SDK gives them (their home). While the
 *    game's setting asks for four cores (RexSwitchCore3SetActive, bo2zm_cpu_cores) they run where the rule says and
 *    keep it whatever the SDK sets later (svcSetThreadPriority is wrapped too); the setting at 3 brings them home at
 *    once. Nothing needs a restart.
 *  - A meter thread on core 3 reads that core's idle time every second (InfoType_IdleTickCount answers only for the
 *    core the caller runs on) and the CPU time of our threads pinned there: what is left is the system's.
 *
 * With a normal launch core 3 is not in the process mask: nothing is kept, the setting changes nothing and the meter
 * does not start.
 */

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
bool RexSwitchCore3SetActive(bool on);  // below; the failsafe in the meter calls it
Result __real_svcSetThreadCoreMask(Handle handle, s32 core_id, u32 affinity_mask);
Result __real_svcSetThreadPriority(Handle handle, u32 priority);
void RexSwitchPerfSetThreadName(u32 handle, const char* name);
size_t RexSwitchPerfThreadTicks(u32* handles, char (*names)[32], u64* ticks, size_t max);
}

namespace {

constexpr u32 kCore3 = 1u << 3;
constexpr int kAudioPriority = 0x2B;  // the SDK's audio threads (REX_SWITCH_PRIO_AUDIO)
constexpr int kGuestPriority = 0x3B;  // the game's threads (REX_SWITCH_PRIO_GUEST)
constexpr size_t kMaxRules = 16;
constexpr size_t kMaxPlaced = 32;
constexpr size_t kMaxThreads = 160;  // switch_perf.cpp's kMaxThreads

struct Rule {
  char prefix[32];
  u32 mask;      // cores the thread may run on (with core 3)
  int priority;  // 0 = left as it is
};
Rule g_rules[kMaxRules];
std::atomic<size_t> g_rule_count{0};

// A thread a rule names. It is on core 3 only while the four cores are on (RexSwitchCore3SetActive) and the process
// has core 3; otherwise it runs where the SDK puts it, which the wrappers keep as its home (core 3 taken out).
struct Placed {
  std::atomic<u32> handle{0};
  u32 mask = 0;                       // the rule's cores (with core 3), within the process's
  int priority = 0;                   // the rule's priority, 0 = the thread's own
  std::atomic<u32> home_mask{0};      // where the SDK wants it (without core 3)
  std::atomic<int> home_priority{0};  // the priority the SDK gave it
  std::atomic<bool> applied{false};   // on core 3 now
  char name[32] = {};
};
Placed g_placed[kMaxPlaced];
std::atomic<size_t> g_placed_count{0};
std::mutex g_placed_mutex;  // moving the threads (switch on / off) against the SDK setting them meanwhile

// bo2zm_cpu_cores (the game's setting, Extras > EXPERIMENTAL): 4 = the rules apply, when the process has core 3.
std::atomic<bool> g_active{false};

std::atomic<bool> g_clamp{true};
std::atomic<u64> g_clamped{0};  // masks that lost core 3
void (*g_log)(const char*) = nullptr;

// The meter's totals, in system ticks.
std::atomic<u64> g_elapsed{0}, g_busy{0}, g_ours{0};
std::atomic<u32> g_worst_permille{0};  // the busiest second since RexSwitchCore3Totals last read it
std::atomic<bool> g_meter_running{false};

u64 ProcessMask() {
  static std::atomic<u64> mask{0};
  u64 m = mask.load(std::memory_order_relaxed);
  if (m == 0) {
    if (R_FAILED(svcGetInfo(&m, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || m == 0) {
      m = 0x7;
    }
    mask.store(m, std::memory_order_relaxed);
  }
  return m;
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
  if (!g_log) {
    return;
  }
  char text[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  g_log(text);
}

u32 Real(Handle handle) {
  return handle == CUR_THREAD_HANDLE ? threadGetCurHandle() : handle;
}

Placed* FindPlaced(u32 handle) {
  const size_t n = std::min(g_placed_count.load(std::memory_order_acquire), kMaxPlaced);
  for (size_t i = 0; i < n; ++i) {
    if (g_placed[i].handle.load(std::memory_order_acquire) == handle) {
      return &g_placed[i];
    }
  }
  return nullptr;
}

s32 Lowest(u32 mask) {
  return s32(__builtin_ctz(mask));
}

// The kernel refuses a mask without the thread's ideal core when the ideal core is kept (-1): then the lowest core of
// the mask becomes the ideal one.
Result SetMask(Handle handle, s32 core_id, u32 mask) {
  Result rc = __real_svcSetThreadCoreMask(handle, core_id, mask);
  if (R_FAILED(rc) && core_id == -1) {
    rc = __real_svcSetThreadCoreMask(handle, Lowest(mask), mask);
  }
  return rc;
}

bool Available() {
  return (ProcessMask() & kCore3) != 0;
}

// The rules apply now: the setting asks for four cores and the process has core 3.
bool Applies() {
  return g_active.load(std::memory_order_acquire) && Available();
}

u32 HomeMask(const Placed& slot) {
  const u32 home = slot.home_mask.load(std::memory_order_relaxed) & ~kCore3;
  return home ? home : u32(ProcessMask()) & ~kCore3;
}

// Under g_placed_mutex. Only core 3: start there. Shared: keep the ideal core; the kernel moves the thread to core 3
// when that core idles.
bool Apply(Placed& slot) {
  const u32 handle = slot.handle.load(std::memory_order_relaxed);
  const Result rc_mask = SetMask(handle, slot.mask == kCore3 ? 3 : -1, slot.mask);
  Result rc_priority = 0;
  if (slot.priority > 0) {
    rc_priority = __real_svcSetThreadPriority(handle, u32(slot.priority));
  }
  slot.applied.store(R_SUCCEEDED(rc_mask), std::memory_order_relaxed);
  return R_SUCCEEDED(rc_mask) && R_SUCCEEDED(rc_priority);
}

// Under g_placed_mutex: back where the SDK wants the thread, at its own priority.
bool Restore(Placed& slot) {
  const u32 handle = slot.handle.load(std::memory_order_relaxed);
  const u32 home = HomeMask(slot);
  const Result rc_mask = SetMask(handle, Lowest(home), home);
  Result rc_priority = 0;
  if (slot.priority > 0 && slot.home_priority.load(std::memory_order_relaxed) > 0) {
    rc_priority = __real_svcSetThreadPriority(handle, u32(slot.home_priority.load(std::memory_order_relaxed)));
  }
  slot.applied.store(false, std::memory_order_relaxed);
  return R_SUCCEEDED(rc_mask) && R_SUCCEEDED(rc_priority);
}

// A thread a rule names: kept in a slot with where the SDK has put it so far, and moved if the rules apply now.
void Place(u32 handle, const char* name, const Rule& rule) {
  if (!Available()) {
    return;  // a normal launch: core 3 can never be used, so nothing is kept and the wrappers leave every thread be
  }
  std::lock_guard<std::mutex> lock(g_placed_mutex);
  Placed* slot = FindPlaced(handle);
  if (!slot) {
    // The handle goes in last, so a slot is only found once it is filled.
    const size_t i = g_placed_count.load(std::memory_order_relaxed);
    if (i >= kMaxPlaced) {
      Log("[core3] %s: not kept, %zu threads already are", name, kMaxPlaced);
      return;
    }
    slot = &g_placed[i];
    slot->mask = (rule.mask & u32(ProcessMask())) | kCore3;
    slot->priority = rule.priority;
    s32 core = 0;
    u64 mask = 0;
    s32 priority = 0;
    slot->home_mask.store(R_SUCCEEDED(svcGetThreadCoreMask(&core, &mask, handle)) ? u32(mask) : 0,
                          std::memory_order_relaxed);
    slot->home_priority.store(R_SUCCEEDED(svcGetThreadPriority(&priority, handle)) ? priority : 0,
                              std::memory_order_relaxed);
    std::strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->handle.store(handle, std::memory_order_release);
    g_placed_count.store(i + 1, std::memory_order_release);
  }
  if (!Applies()) {
    return;  // switched on later (RexSwitchCore3SetActive), if ever
  }
  const bool ok = Apply(*slot);
  Log("[core3] %s: on core 3 (cores mask 0x%X, priority %s%d)%s", name, slot->mask,
      slot->priority > 0 ? "" : "kept ", slot->priority, ok ? "" : " (REFUSED)");
}

const Rule* RuleFor(const char* name) {
  const size_t n = std::min(g_rule_count.load(std::memory_order_acquire), kMaxRules);
  for (size_t i = 0; i < n; ++i) {
    if (std::strncmp(name, g_rules[i].prefix, std::strlen(g_rules[i].prefix)) == 0) {
      return &g_rules[i];
    }
  }
  return nullptr;
}

// "prefix[:mask[:priority]], ...": mask 0x8 (core 3 alone) when left out, priority kept when left out or 0.
void ParseRules(const char* text) {
  size_t n = 0;
  const char* p = text ? text : "";
  while (*p && n < kMaxRules) {
    const char* end = std::strchr(p, ',');
    if (!end) {
      end = p + std::strlen(p);
    }
    char entry[64]{};
    std::memcpy(entry, p, std::min<size_t>(size_t(end - p), sizeof(entry) - 1));
    p = *end ? end + 1 : end;
    char* fields[3] = {entry, nullptr, nullptr};
    for (int f = 1; f < 3; ++f) {
      char* colon = fields[f - 1] ? std::strchr(fields[f - 1], ':') : nullptr;
      if (colon) {
        *colon = 0;
        fields[f] = colon + 1;
      }
    }
    char* prefix = fields[0];
    while (*prefix == ' ') {
      ++prefix;
    }
    size_t length = std::strlen(prefix);
    while (length && prefix[length - 1] == ' ') {
      prefix[--length] = 0;
    }
    if (!length) {
      continue;
    }
    Rule& rule = g_rules[n++];
    std::memset(rule.prefix, 0, sizeof(rule.prefix));
    std::strncpy(rule.prefix, prefix, sizeof(rule.prefix) - 1);
    rule.mask = fields[1] ? u32(std::strtoul(fields[1], nullptr, 0)) & 0xF : kCore3;
    if (!(rule.mask & kCore3)) {
      rule.mask |= kCore3;
    }
    rule.priority = fields[2] ? int(std::strtol(fields[2], nullptr, 0)) : 0;
    // A thread that may also run on cores 0-2 below the game's own band (0x3B) gets nothing there: the game's threads
    // spin on those cores, and the kernel time-slices only 0x3B there (on a Switch Lite, the Server thread allowed on
    // all four cores at priority 63 went silent). It keeps 0x3B.
    if (rule.mask != kCore3 && rule.priority > kGuestPriority) {
      Log("[core3] rule \"%s\": priority %d on cores 0-2 too would starve it there: %d", rule.prefix, rule.priority,
          kGuestPriority);
      rule.priority = kGuestPriority;
    }
  }
  // Threads pinned to core 3 alone at different priorities can lock the game up: the game's threads spin while they
  // wait, so a higher one spinning for something a lower one must do keeps that one from ever running there (the
  // Server at 0x3B over the mixer and the XMA decoder at 63 gave 0 FPS from the start of a match). Only the audio band
  // (0x2B and above) may sit over the others: the SDK's audio threads block between sound blocks instead of spinning
  // (the mixer and the XMA decoder at 0x2B over the Server at 63 run cleanly). Otherwise all of them run at 63, the
  // priority the kernel time-slices on core 3.
  int lowest = -1;  // the numerically highest (least urgent) priority among the core-3-only rules
  bool unknown = false;
  for (size_t i = 0; i < n; ++i) {
    if (g_rules[i].mask == kCore3) {
      if (g_rules[i].priority <= 0) {
        unknown = true;  // kept: whatever the thread has, which this cannot know here
      } else {
        lowest = std::max(lowest, g_rules[i].priority);
      }
    }
  }
  bool unsafe = false;
  for (size_t i = 0; i < n; ++i) {
    if (g_rules[i].mask == kCore3) {
      const int p = g_rules[i].priority;
      const bool above = p <= 0 ? lowest != -1 : p < lowest;  // runs before another thread pinned there
      unsafe |= above && (p <= 0 || p > kAudioPriority);
      unsafe |= unknown && p > 0 && p != lowest;
    }
  }
  if (unsafe) {
    Log("[core3] threads pinned to core 3 alone at different priorities, one of them above the others outside the "
        "audio band (a spinning higher one starves the lower ones there): all of them get 63");
    for (size_t i = 0; i < n; ++i) {
      if (g_rules[i].mask == kCore3) {
        g_rules[i].priority = 63;
      }
    }
  }
  g_rule_count.store(n, std::memory_order_release);
}

// The CPU time of our threads that may only run on core 3: the threads placed with mask 0x8, and the meter. A retail
// kernel keeps no time per core (InfoType_ThreadTickCount by core reads 0: KThread::GetCpuTime(core_id) in
// Atmosphère's kern_k_thread.hpp, "Debug kernels track per-core tick counts"), so a thread that may also run on cores
// 0-2 cannot be split: its core-3 share counts as the system's.
u64 OursOnCore3() {
  u64 sum = 0;
  u64 t = 0;
  if (R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, threadGetCurHandle(), UINT64_MAX))) {
    sum += t;
  }
  const size_t n = std::min(g_placed_count.load(std::memory_order_acquire), kMaxPlaced);
  for (size_t i = 0; i < n; ++i) {
    const u32 handle = g_placed[i].handle.load(std::memory_order_acquire);
    if (handle && g_placed[i].mask == kCore3 && g_placed[i].applied.load(std::memory_order_relaxed) &&
        R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, handle, UINT64_MAX))) {
      sum += t;
    }
  }
  return sum;
}

u64 IdleHere() {
  u64 idle = 0;
  svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, UINT64_MAX);
  return idle;
}

Thread g_meter;

// The failsafe (the meter, above the audio threads at 0x2A, so it runs whatever they do): our threads pinned to core 3
// holding more than g_hog_percent of it for g_hog_seconds seconds in a row switch the four cores off, every named
// thread back on cores 0-2, so the system's services there run again. Without it a Switch Lite once froze whole (a
// hard reset): the Server could run nowhere and the threads pinned to core 3 above the system's services held it,
// probably spinning while they waited for the Server. In normal play the pinned threads take ~30 % of core 3.
std::atomic<int> g_hog_percent{85};
std::atomic<int> g_hog_seconds{2};
std::atomic<bool> g_tripped{false};

void MeterMain(void*) {
  RexSwitchPerfSetThreadName(threadGetCurHandle(), "core 3 meter");
  u64 last_tick = armGetSystemTick(), last_idle = IdleHere(), last_ours = OursOnCore3();
  u64 window_elapsed = 0, window_busy = 0, window_ours = 0;
  u32 window_worst = 0;
  unsigned seconds = 0;
  int hogged_seconds = 0;
  for (;;) {
    svcSleepThread(1'000'000'000LL);
    const u64 tick = armGetSystemTick(), idle = IdleHere(), ours = OursOnCore3();
    const u64 elapsed = tick - last_tick;
    const u64 idle_delta = std::min(idle - last_idle, elapsed);
    const u64 busy = elapsed - idle_delta;
    const u64 ours_delta = ours >= last_ours ? std::min(ours - last_ours, busy) : 0;  // a thread that ended
    last_tick = tick;
    last_idle = idle;
    last_ours = ours;
    if (!elapsed) {
      continue;
    }
    const u32 permille = u32(busy * 1000 / elapsed);
    const u64 ours_percent = ours_delta * 100 / elapsed;
    hogged_seconds = Applies() && ours_percent > u64(g_hog_percent.load(std::memory_order_relaxed))
                         ? hogged_seconds + 1
                         : 0;
    if (hogged_seconds >= g_hog_seconds.load(std::memory_order_relaxed)) {
      hogged_seconds = 0;
      g_tripped.store(true, std::memory_order_release);
      RexSwitchCore3SetActive(false);
      Log("[core3] FAILSAFE: our threads held core 3 (%llu %% in the last second, limit %d %% for %d s): the four "
          "cores are switched off, every thread back on cores 0-2; CPU CORES 4 in the menu switches them on again",
          (unsigned long long)ours_percent, g_hog_percent.load(), g_hog_seconds.load());
    }
    g_elapsed.fetch_add(elapsed, std::memory_order_relaxed);
    g_busy.fetch_add(busy, std::memory_order_relaxed);
    g_ours.fetch_add(ours_delta, std::memory_order_relaxed);
    u32 worst = g_worst_permille.load(std::memory_order_relaxed);
    while (permille > worst && !g_worst_permille.compare_exchange_weak(worst, permille)) {
    }
    window_elapsed += elapsed;
    window_busy += busy;
    window_ours += ours_delta;
    window_worst = std::max(window_worst, permille);
    if (++seconds % 10 == 0) {
      const double e = double(window_elapsed);
      Log("[core3] last 10 s: core 3 busy %.1f %% (our threads pinned there %.1f %%, the rest %.1f %%: the system, "
          "and any thread of ours that may also run elsewhere), busiest second %.1f %%; %llu core masks kept off "
          "core 3 so far",
          100.0 * double(window_busy) / e, 100.0 * double(window_ours) / e,
          100.0 * double(window_busy - window_ours) / e, double(window_worst) / 10.0,
          (unsigned long long)g_clamped.load(std::memory_order_relaxed));
      window_elapsed = window_busy = window_ours = 0;
      window_worst = 0;
    }
  }
}

}  // namespace

extern "C" {

Result __wrap_svcSetThreadCoreMask(Handle handle, s32 core_id, u32 affinity_mask) {
  if (FindPlaced(Real(handle))) {
    // A thread a rule names: what the SDK asks for is its home; on core 3 it keeps its place there.
    std::lock_guard<std::mutex> lock(g_placed_mutex);
    Placed& slot = *FindPlaced(Real(handle));
    slot.home_mask.store(affinity_mask & ~kCore3, std::memory_order_relaxed);
    if (Applies()) {
      return SetMask(handle, slot.mask == kCore3 ? 3 : -1, slot.mask);
    }
    const u32 home = HomeMask(slot);
    const bool keep_core = core_id < 0 || (core_id != 3 && (home & (1u << core_id)) != 0);
    return SetMask(handle, keep_core ? core_id : Lowest(home), home);
  }
  if ((affinity_mask & kCore3) && g_clamp.load(std::memory_order_relaxed)) {
    g_clamped.fetch_add(1, std::memory_order_relaxed);
    u32 mask = affinity_mask & ~kCore3;
    if (mask == 0) {
      mask = u32(ProcessMask()) & ~kCore3;
    }
    if (core_id == 3) {
      core_id = Lowest(mask);
    }
    return SetMask(handle, core_id, mask);
  }
  return __real_svcSetThreadCoreMask(handle, core_id, affinity_mask);
}

Result __wrap_svcSetThreadPriority(Handle handle, u32 priority) {
  if (FindPlaced(Real(handle))) {
    // What the SDK asks for is the thread's own priority; on core 3 the rule's wins.
    std::lock_guard<std::mutex> lock(g_placed_mutex);
    Placed& slot = *FindPlaced(Real(handle));
    slot.home_priority.store(int(priority), std::memory_order_relaxed);
    if (Applies() && slot.priority > 0) {
      priority = u32(slot.priority);
    }
  }
  return __real_svcSetThreadPriority(handle, priority);
}

// The game's setting (bo2zm_cpu_cores): true moves the threads the rules name to core 3, false brings them back to
// cores 0-2 at their own priorities, both at once. Without core 3 (a normal launch) nothing moves either way.
// Returns whether the rules apply now.
bool RexSwitchCore3SetActive(bool on) {
  if (on) {
    g_tripped.store(false, std::memory_order_release);  // the player switches them on again
  }
  if (g_active.exchange(on, std::memory_order_acq_rel) == on) {
    return Applies();
  }
  if (!Available()) {
    if (on) {
      Log("[core3] four cores asked for, but this launch has cores 0-2 (no 4-core forwarder): nothing moves");
    }
    return false;
  }
  int moved = 0, refused = 0;
  {
    std::lock_guard<std::mutex> lock(g_placed_mutex);
    const size_t n = std::min(g_placed_count.load(std::memory_order_acquire), kMaxPlaced);
    for (size_t i = 0; i < n; ++i) {
      if (g_placed[i].handle.load(std::memory_order_acquire)) {
        (on ? Apply(g_placed[i]) : Restore(g_placed[i])) ? ++moved : ++refused;
      }
    }
  }
  Log("[core3] four cores %s: %d thread(s) %s%s", on ? "on" : "off", moved,
      on ? "moved to core 3" : "back on cores 0-2 at their own priorities",
      refused ? " (some moves REFUSED)" : "");
  return on;
}

// Whether the threads the rules name are on core 3 now (the setting at 4 and a 4-core launch).
bool RexSwitchCore3Active(void) {
  return Applies();
}

// Whether the failsafe switched the four cores off (until the setting switches them on again).
bool RexSwitchCore3Tripped(void) {
  return g_tripped.load(std::memory_order_acquire);
}

// The failsafe's limit: our pinned threads above `percent` of core 3 for `seconds` seconds in a row (test tomls lower
// it to see it act).
void RexSwitchCore3SetHogLimit(int percent, int seconds) {
  g_hog_percent.store(std::clamp(percent, 1, 100), std::memory_order_relaxed);
  g_hog_seconds.store(std::max(seconds, 1), std::memory_order_relaxed);
}

// pthread_setname_np (switch_libc_supplement.c): a thread naming itself.
void RexSwitchCore3OnThreadName(u32 handle, const char* name) {
  if (!handle || !name) {
    return;
  }
  if (const Rule* rule = RuleFor(name)) {
    Place(handle, name, *rule);
  }
}

void RexSwitchCore3SetLog(void (*log)(const char*)) {
  g_log = log;
}

// rules: see ParseRules. clamp: keep every other thread off core 3. meter: measure core 3 (when it is ours). active:
// the setting at start (RexSwitchCore3SetActive changes it later).
bool RexSwitchCore3Configure(const char* rules, bool clamp, bool meter, bool active) {
  const u64 process = ProcessMask();
  u64 priorities = 0;
  svcGetInfo(&priorities, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0);
  const bool available = (process & kCore3) != 0;
  g_clamp.store(clamp, std::memory_order_relaxed);
  g_active.store(active, std::memory_order_release);
  Log("[core3] process cores mask 0x%llX (%s), priorities 0x%016llX%s; four cores %s", (unsigned long long)process,
      available ? "core 3 available: a 4-core forwarder" : "cores 0-2: a normal launch",
      (unsigned long long)priorities,
      available ? (clamp ? "; threads kept on cores 0-2 unless a rule moves them" :
                           "; NOT clamped: every thread may use core 3") : "",
      !active ? "off (the setting)" : available ? "on" : "asked for, but not possible with this launch");
  if (available && clamp) {
    // The forwarder's loader gave the main thread the whole process mask; the others get it through the wrapper.
    __wrap_svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, u32(process));
  }
  ParseRules(rules);
  for (size_t i = 0; i < std::min(g_rule_count.load(), kMaxRules); ++i) {
    Log("[core3] rule %zu: threads named \"%s...\" -> cores mask 0x%X, priority %d%s", i, g_rules[i].prefix,
        g_rules[i].mask, g_rules[i].priority, g_rules[i].priority > 0 ? "" : " (kept)");
  }
  if (g_rule_count.load() && available) {
    // Threads that named themselves before the rules arrived.
    static u32 handles[kMaxThreads];
    static char names[kMaxThreads][32];
    static u64 ticks[kMaxThreads];
    const size_t n = RexSwitchPerfThreadTicks(handles, names, ticks, kMaxThreads);
    for (size_t i = 0; i < n; ++i) {
      if (const Rule* rule = RuleFor(names[i])) {
        Place(handles[i], names[i], *rule);
      }
    }
  }
  if (available && meter && !g_meter_running.exchange(true)) {
    if (R_FAILED(threadCreate(&g_meter, MeterMain, nullptr, nullptr, 0x4000, 0x2A, 3)) ||
        R_FAILED(threadStart(&g_meter))) {
      g_meter_running = false;
      Log("[core3] the meter thread did not start");
    }
  }
  return available;
}

// For the profiler's report: the meter's totals (system ticks) and the busiest second since the last call.
bool RexSwitchCore3Totals(u64* elapsed, u64* busy, u64* ours, u32* worst_permille, u64* clamped) {
  if (!g_meter_running.load(std::memory_order_relaxed)) {
    return false;
  }
  *elapsed = g_elapsed.load(std::memory_order_relaxed);
  *busy = g_busy.load(std::memory_order_relaxed);
  *ours = g_ours.load(std::memory_order_relaxed);
  *worst_permille = g_worst_permille.exchange(0, std::memory_order_relaxed);
  *clamped = g_clamped.load(std::memory_order_relaxed);
  return true;
}

}  // extern "C"
