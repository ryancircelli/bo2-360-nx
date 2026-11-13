// A trace of BO2's Direct3D layer (docs/phase4-native-renderer.md).
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// BO2 links the XDK Direct3D library statically, in 0x82160000-0x82190000:
// Swap = sub_821715A8 (calls VdSwap), device/ring setup = sub_82161EA8 (calls
// VdInitializeRingBuffer), ring-space reserve = sub_82161700. Which function is
// DrawIndexedVertices, SetTexture, Resolve... has to be found again for this
// XDK version. Instead of guessing, this wraps every function of the range that
// game code calls, plus every one that writes ring packets
// (bo2zm_d3d_trace_list.inc, from tools/gen_d3d_trace_list.py), and measures
// them while the game runs:
//   - calls per frame (Swap marks frames),
//   - the most frequent callers,
//   - the most frequent values of r4 (the first argument after the device:
//     primitive type, render state, register...),
//   - registers of the first calls.
// The report goes to rex_d3d.log next to the executable every 10 s. Off by
// default (bo2zm_d3d_trace); when off, each hook costs one relaxed load.
//
// Two consistency checks ride along:
//   - Shader registration (sub_82169BF8 VS, sub_821698D8 PS; r3 = object,
//     r4 = physical part): the container must sit at object+0x368 / +0x28 and
//     the physical pointer must land in object+0x20 / +0x18. Every object
//     passed to SetVertexShader/SetPixelShader should have been registered.
//     With bo2zm_d3d_trace_shaders, each registered shader is also written as
//     container || physical (the shader library's blob) to d3d_shaders/.
//   - Command buffers: Begin (sub_8216D5C8) and Run (sub_8216E110) per buffer,
//     to see whether a recorded buffer is ever run twice.
//
// Timing mode (bo2zm_d3d_timing): the same wrappers time every call with the
// cycle counter instead, to measure the CPU the game spends inside its Direct3D
// layer. Nesting is tracked per thread, so each function gets its inclusive and
// exclusive time (exclusive = minus the wrapped functions it calls), and the
// outermost calls give the D3D time of each thread. Report: rex_d3d_timing.log
// next to the executable, every 10 s. bo2zm_d3d_trace wins if both are on.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "bo2zm_benchmark.h"
#include "bo2zm_d3d_native.h"
#include "native/render/bo2zm_native_hooks.h"

REXCVAR_DEFINE_BOOL(bo2zm_d3d_trace, false, "BO2",
                    "Trace the game's Direct3D calls to rex_d3d.log")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(bo2zm_d3d_trace_shaders, false, "BO2",
                    "With bo2zm_d3d_trace: write each registered shader (container + microcode) to d3d_shaders/")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(bo2zm_d3d_timing, false, "BO2",
                    "Time the game's Direct3D calls per thread and per function: rex_d3d_timing.log")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// A measuring aid for the D3D renderer plan: how much the game threads' D3D time limits the frame rate. With the
// benchmark A/B (bo2zm_benchmark_ab_s), each DrawIndexedVertices spins this many nanoseconds first, on the thread that
// calls it, in the arms where factor 2 is on.
REXCVAR_DEFINE_INT32(bo2zm_d3d_spin_ns_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: each DrawIndexedVertices spins this many ns on the calling thread "
                     "in the arms of A/B factor 2 (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace bo2zm::d3d_trace {
namespace {

constexpr uint32_t kSwap = 0x821715A8;

constexpr uint32_t kAddresses[] = {
#define X(a) 0x##a,
#include "bo2zm_d3d_trace_list.inc"
#undef X
};

enum : uint32_t {
#define X(a) kFn_##a,
#include "bo2zm_d3d_trace_list.inc"
#undef X
  kFnCount
};

constexpr size_t kMaxDetail = 6;     // first calls with registers
constexpr size_t kMaxCallers = 8;    // distinct callers kept
constexpr size_t kMaxValues = 8;     // distinct r4 values kept
constexpr uint64_t kSampled = 50000; // callers/values are counted for the first calls only
constexpr int kReportSeconds = 10;

// The checks (see the header comment).
constexpr uint32_t kRegisterVS = 0x82169BF8;
constexpr uint32_t kRegisterPS = 0x821698D8;
constexpr uint32_t kSetVS = 0x82169DE0;
constexpr uint32_t kSetPS = 0x82169A00;
constexpr uint32_t kBeginCB = 0x8216D5C8;
constexpr uint32_t kRunCB = 0x8216E110;
constexpr uint32_t kContainerOffset[2] = {0x368, 0x28};  // [vertex, pixel]
constexpr uint32_t kPhysicalField[2] = {0x20, 0x18};
// Container flags are 0x102A11xx; bit 0 is set for vertex shaders (CreateVertexShader tests it, 82169D38).
// Other low bits vary: the gpu_transfer memexport vertex shaders have 0x102A1111.
constexpr uint32_t kContainerMagic = 0x102A1100;
constexpr uint32_t kMaxShaderBytes = 64 * 1024;
constexpr uint32_t kMaxDumped = 16384;
constexpr size_t kMaxUnregistered = 8;

struct Detail {
  uint32_t lr;
  uint32_t r[8];  // r3..r10
  double f1;
  uint32_t ret;
};

struct Counted {
  uint32_t key;
  uint64_t n;
};

struct Slot {
  std::atomic<uint64_t> calls{0};
  uint64_t calls_at_report = 0;
  std::atomic<uint32_t> details{0};
  Detail detail[kMaxDetail]{};
  Counted callers[kMaxCallers]{};
  Counted values[kMaxValues]{};
  bool described = false;
};

Slot g_slots[kFnCount];
std::mutex g_mutex;  // callers, values and the report
std::atomic<uint64_t> g_frames{0};
uint64_t g_frames_at_report = 0;
std::atomic<bool> g_started{false};
enum Mode : uint32_t { kOff, kTrace, kTiming };
std::atomic<uint32_t> g_mode{kOff};
std::chrono::steady_clock::time_point g_last_report{};
std::atomic<bool> g_writing{false};

// Check state, under g_mutex. Index 0 = vertex, 1 = pixel.
struct ShaderStats {
  uint64_t registered = 0;
  uint64_t container_ok = 0;  // magic and sizes plausible at object + kContainerOffset
  uint64_t physical_ok = 0;   // object + kPhysicalField == r4 after the call
  uint64_t dumped = 0;
  uint64_t sets = 0;          // Set*Shader calls with a non-null object
  uint64_t sets_registered = 0;
  std::vector<uint32_t> unregistered;  // first distinct objects set without registration
};
ShaderStats g_shaders[2];
std::unordered_map<uint32_t, uint8_t> g_registered;  // object -> 1 + kind
uint32_t g_dump_sequence = 0;

struct BufferStats {
  uint64_t begins = 0;
  uint64_t runs = 0;
  uint32_t runs_since_begin = 0;
  uint32_t max_runs_per_begin = 0;
};
std::unordered_map<uint32_t, BufferStats> g_buffers;
uint64_t g_runs_before_begin = 0;

std::string ReportPath(const char* name = "rex_d3d.log") {
  const auto folder = rex::filesystem::GetExecutableFolder();
#if defined(__SWITCH__)
  // Beside rex_profile.log, with the other logs.
  if (!folder.empty()) {
    return (folder / "logs" / "rex" / name).string();
  }
#endif
  return folder.empty() ? std::string(name) : (folder / name).string();
}

namespace timing {
void Begin();
}

[[gnu::noinline, gnu::cold]] void StartSlow() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) {
    return;
  }
  if (!REXCVAR_GET(bo2zm_d3d_trace)) {
    if (REXCVAR_GET(bo2zm_d3d_timing)) {
      timing::Begin();
      g_mode.store(kTiming, std::memory_order_release);
    }
    return;
  }
  if (FILE* f = std::fopen(ReportPath().c_str(), "w")) {
    std::fprintf(f, "BO2 Direct3D trace: %zu functions wrapped\n\n", size_t(kFnCount));
    std::fclose(f);
  }
  REXLOG_INFO("[d3d] tracing {} Direct3D functions to {}", size_t(kFnCount), ReportPath());
  g_mode.store(kTrace, std::memory_order_release);
}

inline void Start() {
  if (!g_started.load(std::memory_order_relaxed)) [[unlikely]] {
    StartSlow();
  }
}

void Count(Counted (&table)[kMaxCallers], uint32_t key) {
  for (auto& c : table) {
    if (c.n == 0) {
      c.key = key;
      c.n = 1;
      return;
    }
    if (c.key == key) {
      ++c.n;
      return;
    }
  }
}

template <typename Ctx>
void Enter(uint32_t id, const Ctx& ctx) {
  Slot& s = g_slots[id];
  const uint64_t n = s.calls.fetch_add(1, std::memory_order_relaxed);
  if (n < kSampled) {
    std::lock_guard lock(g_mutex);
    Count(s.callers, uint32_t(ctx.lr));
    Count(s.values, ctx.r4.u32);
  }
}

template <typename Ctx>
void Leave(uint32_t id, const Ctx& ctx, uint32_t lr, const uint32_t (&r)[8], double f1) {
  Slot& s = g_slots[id];
  const uint32_t d = s.details.load(std::memory_order_relaxed);
  if (d < kMaxDetail) {
    std::lock_guard lock(g_mutex);
    const uint32_t k = s.details.load(std::memory_order_relaxed);
    if (k < kMaxDetail) {
      Detail& det = s.detail[k];
      det.lr = lr;
      std::copy(std::begin(r), std::end(r), det.r);
      det.f1 = f1;
      det.ret = ctx.r3.u32;
      s.details.store(k + 1, std::memory_order_relaxed);
    }
  }
}

uint32_t LoadBE32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

// Container sizes at object + kContainerOffset, or false when they don't look like an XDK container.
bool ContainerAt(const uint8_t* base, uint32_t object, int kind, uint32_t& virtual_size, uint32_t& physical_size) {
  const uint64_t container = uint64_t(object) + kContainerOffset[kind];
  if (!object || container + 24 > (uint64_t(1) << 32)) {
    return false;
  }
  const uint32_t flags = LoadBE32(base, uint32_t(container));
  if ((flags & 0xFFFFFF00) != kContainerMagic || (flags & 1) != uint32_t(kind == 0)) {
    return false;
  }
  virtual_size = LoadBE32(base, uint32_t(container + 4));
  physical_size = LoadBE32(base, uint32_t(container + 8));
  return virtual_size >= 24 && virtual_size <= kMaxShaderBytes && physical_size <= kMaxShaderBytes &&
         container + virtual_size <= (uint64_t(1) << 32);
}

// Writes container || physical, the same blob tools/inspect/ff_shaders.py --dump makes from the fastfile.
void DumpShader(const uint8_t* base, uint32_t object, uint32_t physical, int kind, uint32_t virtual_size,
                uint32_t physical_size) {
  if (g_dump_sequence >= kMaxDumped || !physical || uint64_t(physical) + physical_size > (uint64_t(1) << 32)) {
    return;
  }
  const auto folder = rex::filesystem::GetExecutableFolder();
  if (folder.empty()) {
    return;
  }
  const auto dir = folder / "d3d_shaders";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return;
  }
  char name[48];
  std::snprintf(name, sizeof(name), "%05u_%c_%08X.bin", g_dump_sequence++, kind ? 'p' : 'v', object);
  if (FILE* f = std::fopen((dir / name).string().c_str(), "wb")) {
    const bool ok = std::fwrite(base + object + kContainerOffset[kind], 1, virtual_size, f) == virtual_size &&
                    std::fwrite(base + physical, 1, physical_size, f) == physical_size;
    if (std::fclose(f) == 0 && ok) {
      ++g_shaders[kind].dumped;
    }
  }
}

// Before the original runs: r3/r4 are the arguments. After: the call's effects.
void CheckBefore(uint32_t address, uint32_t r4) {
  if (address == kSetVS || address == kSetPS) {
    const int kind = address == kSetPS;
    if (!r4) {
      return;
    }
    std::lock_guard lock(g_mutex);
    ShaderStats& s = g_shaders[kind];
    ++s.sets;
    if (g_registered.count(r4)) {
      ++s.sets_registered;
    } else if (s.unregistered.size() < kMaxUnregistered &&
               std::find(s.unregistered.begin(), s.unregistered.end(), r4) == s.unregistered.end()) {
      s.unregistered.push_back(r4);
    }
  } else if (address == kBeginCB || address == kRunCB) {
    if (!r4) {
      return;
    }
    std::lock_guard lock(g_mutex);
    BufferStats& b = g_buffers[r4];
    if (address == kBeginCB) {
      ++b.begins;
      b.runs_since_begin = 0;
    } else {
      if (!b.begins) {
        ++g_runs_before_begin;
      }
      ++b.runs;
      b.max_runs_per_begin = std::max(b.max_runs_per_begin, ++b.runs_since_begin);
    }
  }
}

void CheckAfter(uint32_t address, uint32_t r3, uint32_t r4, const uint8_t* base) {
  if (address != kRegisterVS && address != kRegisterPS) {
    return;
  }
  const int kind = address == kRegisterPS;
  uint32_t virtual_size = 0, physical_size = 0;
  const bool container_ok = ContainerAt(base, r3, kind, virtual_size, physical_size);
  const bool physical_ok = r3 && LoadBE32(base, r3 + kPhysicalField[kind]) == r4;
  std::lock_guard lock(g_mutex);
  ShaderStats& s = g_shaders[kind];
  ++s.registered;
  s.container_ok += container_ok;
  s.physical_ok += physical_ok;
  g_registered[r3] = uint8_t(1 + kind);
  if (container_ok && REXCVAR_GET(bo2zm_d3d_trace_shaders)) {
    DumpShader(base, r3, r4, kind, virtual_size, physical_size);
  }
}

void PrintChecks(FILE* f) {
  for (int kind = 0; kind < 2; ++kind) {
    const ShaderStats& s = g_shaders[kind];
    std::fprintf(f, "  %s shaders: registered %llu (container ok %llu, physical pointer ok %llu, dumped %llu); "
                    "set %llu times, %llu with a registered object",
                 kind ? "pixel" : "vertex", (unsigned long long)s.registered, (unsigned long long)s.container_ok,
                 (unsigned long long)s.physical_ok, (unsigned long long)s.dumped, (unsigned long long)s.sets,
                 (unsigned long long)s.sets_registered);
    if (!s.unregistered.empty()) {
      std::fprintf(f, "; unregistered:");
      for (uint32_t o : s.unregistered) {
        std::fprintf(f, " %08X", o);
      }
    }
    std::fputc('\n', f);
  }
  uint64_t begins = 0, runs = 0;
  uint32_t max_runs = 0;
  for (const auto& [cb, b] : g_buffers) {
    begins += b.begins;
    runs += b.runs;
    max_runs = std::max(max_runs, b.max_runs_per_begin);
  }
  std::fprintf(f, "  command buffers: %zu distinct, %llu begins, %llu runs, max runs after one begin %u, "
                  "runs before any begin %llu\n",
               g_buffers.size(), (unsigned long long)begins, (unsigned long long)runs, max_runs,
               (unsigned long long)g_runs_before_begin);
}

void PrintCounted(FILE* f, const char* label, const Counted (&table)[kMaxCallers]) {
  Counted sorted[kMaxCallers];
  std::copy(std::begin(table), std::end(table), sorted);
  std::sort(std::begin(sorted), std::end(sorted), [](const Counted& a, const Counted& b) { return a.n > b.n; });
  std::fprintf(f, "     %s:", label);
  for (const auto& c : sorted) {
    if (c.n) {
      std::fprintf(f, " %08X x%llu", c.key, (unsigned long long)c.n);
    }
  }
  std::fputc('\n', f);
}

void WriteReport() {
  FILE* f = std::fopen(ReportPath().c_str(), "a");
  if (!f) {
    return;
  }
  std::lock_guard lock(g_mutex);
  const uint64_t frames = g_frames.load(std::memory_order_relaxed);
  const uint64_t window = frames - g_frames_at_report;
  g_frames_at_report = frames;

  struct Row {
    uint32_t id;
    uint64_t delta;
  };
  std::vector<Row> rows;
  for (uint32_t i = 0; i < kFnCount; ++i) {
    const uint64_t calls = g_slots[i].calls.load(std::memory_order_relaxed);
    rows.push_back({i, calls - g_slots[i].calls_at_report});
    g_slots[i].calls_at_report = calls;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.delta > b.delta; });

  std::fprintf(f, "==== %llu frames total, %llu in this window\n", (unsigned long long)frames,
               (unsigned long long)window);
  PrintChecks(f);
  for (const Row& row : rows) {
    if (!row.delta) {
      continue;
    }
    std::fprintf(f, "  sub_%08X %10llu calls  %9.2f per frame\n", kAddresses[row.id],
                 (unsigned long long)row.delta, window ? double(row.delta) / double(window) : 0.0);
  }

  // Functions seen for the first time: callers, r4 values and first calls, once.
  for (uint32_t i = 0; i < kFnCount; ++i) {
    Slot& s = g_slots[i];
    if (s.described || s.calls.load(std::memory_order_relaxed) == 0) {
      continue;
    }
    s.described = true;
    std::fprintf(f, "\n  -- sub_%08X (%llu calls so far)\n", kAddresses[i],
                 (unsigned long long)s.calls.load(std::memory_order_relaxed));
    PrintCounted(f, "callers", s.callers);
    PrintCounted(f, "r4 values", s.values);
    const uint32_t n = s.details.load(std::memory_order_relaxed);
    for (uint32_t k = 0; k < n; ++k) {
      const Detail& d = s.detail[k];
      std::fprintf(f, "     lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X f1=%g -> %08X\n",
                   d.lr, d.r[0], d.r[1], d.r[2], d.r[3], d.r[4], d.r[5], d.r[6], d.r[7], d.f1, d.ret);
    }
  }
  std::fputc('\n', f);
  std::fclose(f);
}

void OnSwap() {
  g_frames.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now();
  if (g_last_report.time_since_epoch().count() == 0) {
    g_last_report = now;
    return;
  }
  if (now - g_last_report < std::chrono::seconds(kReportSeconds)) {
    return;
  }
  bool expected = false;
  if (!g_writing.compare_exchange_strong(expected, true)) {
    return;
  }
  g_last_report = now;
  WriteReport();
  g_writing.store(false);
}

// ---- Timing mode (bo2zm_d3d_timing) ----
namespace timing {

// The game's GPU busy-wait calls these three in a loop:
// device ownership acquire/release and ring status. Outermost calls to them are reported as "spin", not work.
constexpr uint32_t kSpin[] = {0x82170288, 0x821702C8, 0x82171DD8};
constexpr int kMaxThreads = 64;
constexpr int kMaxDepth = 32;

inline uint64_t Ticks() {
#if defined(__aarch64__)
  // cntpct_el0, as libnx's armGetSystemTick: Horizon traps EL0 reads of cntvct_el0 (the process dies on the first
  // timed call, ESR EC 0x18 at `mrs x3, cntvct_el0`).
  uint64_t t;
  asm volatile("mrs %0, cntpct_el0" : "=r"(t));
  return t;
#elif defined(__x86_64__) || defined(_M_X64)
  return __rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// One thread's counters. Only that thread writes them (no atomic read-modify-write, which is a loop on the
// A57); the report reads them with relaxed loads.
struct Thread {
  std::atomic<bool> used{false};
  std::atomic<bool> named{false};
  uint32_t id = 0;     // guest thread id
  uint32_t tries = 0;  // name lookups (owner only)
  char name[48]{};
  std::atomic<uint64_t> work_ticks{0}, work_calls{0};  // outermost calls, spin excluded
  std::atomic<uint64_t> spin_ticks{0}, spin_calls{0};
  std::atomic<uint64_t> incl[kFnCount]{}, excl[kFnCount]{}, calls[kFnCount]{}, outer[kFnCount]{};
  uint64_t at_report[4]{};  // work ticks/calls, spin ticks/calls at the previous report (reporter only)
};
Thread g_threads[kMaxThreads];
std::atomic<int> g_thread_count{0};
thread_local int t_slot = -1;
thread_local int t_depth = 0;
thread_local uint64_t t_child[kMaxDepth];

bool g_spin[kFnCount];
std::atomic<uint64_t> g_frames{0};
uint64_t g_frames_at_report = 0;
uint64_t g_fn_at_report[4][kFnCount];  // incl, excl, calls, outer summed over threads
std::chrono::steady_clock::time_point g_clock0, g_last_report;
uint64_t g_ticks0 = 0;
double g_timer_cost_ticks = 0;  // two Ticks(), measured at start
volatile uint64_t g_sink = 0;
std::atomic<bool> g_writing{false};

inline void Add(std::atomic<uint64_t>& c, uint64_t n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

[[gnu::noinline, gnu::cold]] Thread* Register() {
  const int slot = g_thread_count.fetch_add(1, std::memory_order_relaxed);
  if (slot >= kMaxThreads) {
    t_slot = kMaxThreads;  // out of slots: this thread is not timed
    return nullptr;
  }
  Thread& t = g_threads[slot];
  t.id = rex::system::XThread::GetCurrentThreadId();
  t.used.store(true, std::memory_order_release);
  t_slot = slot;
  return &t;
}

// The game names most of its threads after they start, often after their first D3D call, so the name is
// looked up again every 1,024 outermost calls until there is one.
[[gnu::noinline, gnu::cold]] void Name(Thread& t) {
  if ((t.tries++ & 1023) != 0) {
    return;
  }
  // The game's names (Backend, JQ Worker n...) are set on the host thread, not on the XThread object.
  const auto* thread = rex::thread::Thread::GetCurrentThread();
  if (!thread || thread->name().empty()) {
    return;
  }
  std::snprintf(t.name, sizeof(t.name), "%s", thread->name().c_str());
  t.named.store(true, std::memory_order_release);
}

inline Thread* Mine() {
  if (t_slot < 0) [[unlikely]] {
    return Register();
  }
  return t_slot < kMaxThreads ? &g_threads[t_slot] : nullptr;
}

void Begin() {
  for (uint32_t i = 0; i < kFnCount; ++i) {
    g_spin[i] = std::find(std::begin(kSpin), std::end(kSpin), kAddresses[i]) != std::end(kSpin);
  }
  // The cost of one timed call around an empty body, so the report can say how much of each number is ours.
  uint64_t sink = 0;
  constexpr int kLoops = 100000;
  const uint64_t a = Ticks();
  for (int i = 0; i < kLoops; ++i) {
    const uint64_t t0 = Ticks();
    sink += Ticks() - t0;
  }
  g_timer_cost_ticks = double(Ticks() - a) / kLoops;
  g_sink = sink;
  g_clock0 = g_last_report = std::chrono::steady_clock::now();
  g_ticks0 = Ticks();
  if (FILE* f = std::fopen(ReportPath("rex_d3d_timing.log").c_str(), "w")) {
    std::fprintf(f, "BO2 Direct3D timing: %zu functions wrapped; each line is one 10-s window\n\n",
                 size_t(kFnCount));
    std::fclose(f);
  }
  REXLOG_INFO("[d3d] timing {} Direct3D functions to {}", size_t(kFnCount), ReportPath("rex_d3d_timing.log"));
}

void WriteReport() {
  const auto now = std::chrono::steady_clock::now();
  const uint64_t ticks = Ticks();
  const double seconds = std::chrono::duration<double>(now - g_last_report).count();
  const double ticks_per_us =
      double(ticks - g_ticks0) / std::max(1.0, std::chrono::duration<double, std::micro>(now - g_clock0).count());
  g_last_report = now;
  const uint64_t frames_total = g_frames.load(std::memory_order_relaxed);
  const uint64_t frames = frames_total - g_frames_at_report;
  g_frames_at_report = frames_total;
  if (!frames) {
    return;
  }
  const double per_frame_us = 1.0 / (ticks_per_us * double(frames));
  FILE* f = std::fopen(ReportPath("rex_d3d_timing.log").c_str(), "a");
  if (!f) {
    return;
  }
  std::fprintf(f, "==== %.1f s, %llu frames (%.1f fps); %.1f ticks/us; two counter reads cost ~%.0f ns per timed call (included)\n",
               seconds, (unsigned long long)frames, double(frames) / seconds, ticks_per_us,
               g_timer_cost_ticks / ticks_per_us * 1000.0);

  // Threads: outermost D3D calls.
  std::fprintf(f, "  threads (outermost calls; spin = the GPU busy-wait's acquire/release/ring status):\n");
  double work_total = 0;
  uint64_t work_calls_total = 0;
  const int threads = std::min(g_thread_count.load(std::memory_order_relaxed), kMaxThreads);
  for (int i = 0; i < threads; ++i) {
    Thread& t = g_threads[i];
    if (!t.used.load(std::memory_order_acquire)) {
      continue;
    }
    const uint64_t now4[4] = {t.work_ticks.load(std::memory_order_relaxed), t.work_calls.load(std::memory_order_relaxed),
                              t.spin_ticks.load(std::memory_order_relaxed), t.spin_calls.load(std::memory_order_relaxed)};
    uint64_t d[4];
    for (int k = 0; k < 4; ++k) {
      d[k] = now4[k] - t.at_report[k];
      t.at_report[k] = now4[k];
    }
    if (!d[1] && !d[3]) {
      continue;
    }
    work_total += double(d[0]) * per_frame_us;
    work_calls_total += d[1];
    std::fprintf(f, "    %-26s %5X D3D %8.3f ms/frame (%9.1f calls/frame)   spin %8.3f ms/frame (%9.1f calls/frame)\n",
                 t.named.load(std::memory_order_acquire) ? t.name : "(no name)", t.id,
                 double(d[0]) * per_frame_us / 1000.0, double(d[1]) / double(frames),
                 double(d[2]) * per_frame_us / 1000.0, double(d[3]) / double(frames));
  }
  std::fprintf(f, "    %-32s D3D %8.3f ms/frame (%9.1f calls/frame)\n", "all threads      ", work_total / 1000.0,
               double(work_calls_total) / double(frames));

  // Functions, summed over threads.
  struct Row {
    uint32_t id;
    uint64_t d[4];  // incl, excl, calls, outer
  };
  std::vector<Row> rows;
  for (uint32_t id = 0; id < kFnCount; ++id) {
    uint64_t sum[4] = {};
    for (int i = 0; i < threads; ++i) {
      Thread& t = g_threads[i];
      sum[0] += t.incl[id].load(std::memory_order_relaxed);
      sum[1] += t.excl[id].load(std::memory_order_relaxed);
      sum[2] += t.calls[id].load(std::memory_order_relaxed);
      sum[3] += t.outer[id].load(std::memory_order_relaxed);
    }
    Row r{id, {}};
    for (int k = 0; k < 4; ++k) {
      r.d[k] = sum[k] - g_fn_at_report[k][id];
      g_fn_at_report[k][id] = sum[k];
    }
    if (r.d[2]) {
      rows.push_back(r);
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d[1] > b.d[1]; });
  std::fprintf(f, "  functions by exclusive time (us/frame; exclusive = minus the wrapped functions it calls;\n"
                  "  outer = inclusive us/frame of the calls made from outside the wrapped set):\n");
  for (const Row& r : rows) {
    std::fprintf(f, "    sub_%08X%s calls %9.1f/frame  excl %9.1f  incl %9.1f  (%6.3f us/call excl)  outer %9.1f\n",
                 kAddresses[r.id], g_spin[r.id] ? " spin" : "     ", double(r.d[2]) / double(frames),
                 double(r.d[1]) * per_frame_us, double(r.d[0]) * per_frame_us,
                 double(r.d[1]) / std::max<double>(1.0, double(r.d[2])) / ticks_per_us,
                 double(r.d[3]) * per_frame_us);
  }
  std::fputc('\n', f);
  std::fclose(f);
}

void OnSwap() {
  g_frames.fetch_add(1, std::memory_order_relaxed);
  if (std::chrono::steady_clock::now() - g_last_report < std::chrono::seconds(kReportSeconds)) {
    return;
  }
  bool expected = false;
  if (!g_writing.compare_exchange_strong(expected, true)) {
    return;
  }
  WriteReport();
  g_writing.store(false);
}

template <typename Ctx, typename Original>
void Timed(uint32_t id, Ctx& ctx, uint8_t* base, Original original) {
  Thread* t = Mine();
  const int depth = t_depth;
  if (!t || depth >= kMaxDepth) [[unlikely]] {
    original(ctx, base);
    return;
  }
  t_depth = depth + 1;
  t_child[depth] = 0;
  const uint64_t t0 = Ticks();
  original(ctx, base);
  const uint64_t dt = Ticks() - t0;
  t_depth = depth;
  Add(t->incl[id], dt);
  Add(t->excl[id], dt - std::min(dt, t_child[depth]));
  Add(t->calls[id], 1);
  if (depth > 0) {
    t_child[depth - 1] += dt;
    return;
  }
  Add(t->outer[id], dt);
  if (!t->named.load(std::memory_order_relaxed)) [[unlikely]] {
    Name(*t);
  }
  if (g_spin[id]) {
    Add(t->spin_ticks, dt);
    Add(t->spin_calls, 1);
  } else {
    Add(t->work_ticks, dt);
    Add(t->work_calls, 1);
  }
  if (kAddresses[id] == kSwap) {
    OnSwap();
  }
}

}  // namespace timing

template <typename Ctx, typename Original>
void TraceCall(uint32_t id, uint32_t address, Ctx& ctx, uint8_t* base, Original original);

template <typename Ctx, typename Original>
void Trace(uint32_t id, uint32_t address, Ctx& ctx, uint8_t* base, Original original) {
  // D3D renderer M2 step 3: the calls whose inlined flush sends the dirty mirror constants (DrawIndexedVertices,
  // DrawVertices, the UP draw writers and DrawIndexedVerticesUP, the resolve, End and Run) tell the D3D shadow first,
  // while the dirty bits are still set.
  switch (address) {
    case 0x8216B1D0u:
    case 0x8216ADB8u:
    case 0x8216A2D0u:
    case 0x8216A7D8u:
    case 0x8216AD18u:
    case 0x8216BFC8u:
    case 0x8216D110u:
    case 0x8216DB38u:
    case 0x8216E110u:
      bo2zm::native::BeforeFlush(base, ctx.r3.u32);
      break;
    default:
      break;
  }
  // The command buffers (BeginCommandBuffer(device, buffer, ...), EndCommandBuffer(device),
  // RunCommandBuffer(device, buffer, predication)): what a buffer leaves on the GPU goes to the device that runs it.
  if (address == 0x8216D5C8u) {
    bo2zm::native::BeginBuffer(ctx.r3.u32, ctx.r4.u32);
  } else if (address == 0x8216DB38u) {
    bo2zm::native::EndBuffer(ctx.r3.u32);
  } else if (address == 0x8216E110u) {
    const uint32_t device = ctx.r3.u32;
    const uint32_t buffer = ctx.r4.u32;
    TraceCall(id, address, ctx, base, original);
    bo2zm::native::AfterRun(device, buffer);
    return;
  }
  // D3D renderer M2 step 2: DrawIndexedVertices (sub_8216B1D0: type r4, base vertex r5, start index r6, index count
  // r7) puts its record into the command stream first (bo2zm_d3d_marker.cpp, BeforeDraw); the D3D shadow takes its
  // snapshot when the call returns (AfterDraw).
  // Not DrawVertices (sub_8216ADB8): its ~5 calls a frame are the gpu_transfer memexport draws (type 1, 32 or 64
  // vertices, VS BE3EFB70, not in the library), which the ring runs by the memexport path with no shader identity, so
  // their records could never agree with the ring's and would switch phase 2b off at once.
  if (address == 0x8216B1D0u) {
    static const int32_t spin_ns = REXCVAR_GET(bo2zm_d3d_spin_ns_ab);
    if (spin_ns > 0) [[unlikely]] {
      const int arm = bo2zm::benchmark::AbArmCached();
      if (arm >= 0 && (arm & 2)) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin_ns);
        while (std::chrono::steady_clock::now() < end) {
        }
      }
    }
    bo2zm::native::BeforeDraw(base, ctx.r3.u32, bo2zm::native::DrawFunction::kIndexed, ctx.r4.u32, ctx.r5.u32,
                              ctx.r6.u32, ctx.r7.u32);
    // D3D renderer M5: DrawIndexedVertices in native code while nothing is traced (bo2zm_d3d_native.cpp).
    Start();
    if (g_mode.load(std::memory_order_relaxed) != kOff || !bo2zm::d3d_native::DrawIndexedVerticesNative(ctx, base)) {
      TraceCall(id, address, ctx, base, original);
    }
    bo2zm::native::AfterDraw(base);
    return;
  }
  // D3D renderer M5: functions in native code while nothing is traced (bo2zm_d3d_native.cpp).
  if (address == 0x82164190u) {
    Start();
    if (g_mode.load(std::memory_order_relaxed) == kOff && bo2zm::d3d_native::SetTextureNative(ctx, base)) {
      return;
    }
  } else if (address == kSetVS || address == kSetPS) {
    Start();
    if (g_mode.load(std::memory_order_relaxed) == kOff &&
        (address == kSetVS ? bo2zm::d3d_native::SetVertexShaderNative(ctx, base)
                           : bo2zm::d3d_native::SetPixelShaderNative(ctx, base))) {
      return;
    }
  }
  // D3D renderer M2 step 3: sub_82174868(device r3, kind r4, start register r5, Vector4fCount r6) reserves a
  // SET_SHADER_CONSTANTS in the stream and returns its data pointer, which the calling game code fills: note it for
  // the D3D shadow, which reads the values at the device's next draw.
  if (address == 0x82174868u) {
    const uint32_t device = ctx.r3.u32;
    const uint32_t vectors = ctx.r6.u32;
    TraceCall(id, address, ctx, base, original);
    bo2zm::native::NoteGameConstants(base, device, ctx.r3.u32, vectors);
    return;
  }
  TraceCall(id, address, ctx, base, original);
}

template <typename Ctx, typename Original>
void TraceCall(uint32_t id, uint32_t address, Ctx& ctx, uint8_t* base, Original original) {
  Start();
  const uint32_t mode = g_mode.load(std::memory_order_relaxed);
  if (mode == kOff) [[likely]] {
    original(ctx, base);
    return;
  }
  if (mode == kTiming) {
    timing::Timed(id, ctx, base, original);
    return;
  }
  if (address == kSwap) {
    OnSwap();
  }
  Enter(id, ctx);
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;
  CheckBefore(address, r4);
  const bool want_detail = g_slots[id].details.load(std::memory_order_relaxed) < kMaxDetail;
  uint32_t r[8] = {};
  uint32_t lr = 0;
  double f1 = 0;
  if (want_detail) {
    lr = uint32_t(ctx.lr);
    r[0] = ctx.r3.u32, r[1] = ctx.r4.u32, r[2] = ctx.r5.u32, r[3] = ctx.r6.u32;
    r[4] = ctx.r7.u32, r[5] = ctx.r8.u32, r[6] = ctx.r9.u32, r[7] = ctx.r10.u32;
    f1 = ctx.f1.f64;
  }
  original(ctx, base);
  CheckAfter(address, r3, r4, base);
  if (want_detail) {
    Leave(id, ctx, lr, r, f1);
  }
}

}  // namespace
}  // namespace bo2zm::d3d_trace

#define X(a)                                                                                    \
  REX_EXTERN(__imp__sub_##a);                                                                   \
  REX_HOOK_RAW(sub_##a) {                                                                       \
    bo2zm::d3d_trace::Trace(bo2zm::d3d_trace::kFn_##a, 0x##a, ctx, base,                        \
                            [](auto& c, uint8_t* b) { __imp__sub_##a(c, b); });                 \
  }
#include "bo2zm_d3d_trace_list.inc"
#undef X
