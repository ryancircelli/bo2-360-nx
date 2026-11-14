#pragma once
// Per-phase timers for the Metal command-processor ("GPU Commands") thread. Test instrumentation, default off
// (cvar metal_cp_phase_timers / env REX_METAL_CP_PHASE_TIMERS=1); when off every scope is one predictable branch on a
// cached bool and no clock is read.
//
// Phases are exclusive: a nested scope pauses its parent, so the sum of phases never double counts. Time is wall
// time from mach_absolute_time() on the command-processor thread. Whatever the scopes do not cover (the PM4 loop's
// type-3 dispatch, shader analysis, draw state decoding, ...) is "other" = the thread's CPU time for the frame minus
// the phases; the swap row records both so the analysis can join with perf.tsv.
//
// One row per swap is written to cp_phases.tsv next to perf.tsv (the directory of REX_BO2ZM_PERF_LOG, or the file
// named by REX_METAL_CP_PHASE_OUT). Columns (all ns unless noted):
//   frame            swap index, 0-based, the same index as perf.tsv's frame column
//   cpu_ns           command-processor thread CPU time since the previous swap (CLOCK_THREAD_CPUTIME_ID, as perf cpu_ms)
//   parse..resolve   phase times:
//     parse    type-0 / register-range / constant-range writes (WriteRegister and friends)
//     shmem    shared-memory requests (RequestSharedMemoryRanges, upload preparation)
//     tex      texture requests / materialization / upload batches / SRV + sampler lookups in the constant path
//     pipe     pipeline state lookup / creation, shader translation readiness, respecialization requests
//     vtx      primitive processing (index conversion, vertex fetch setup)
//     const    PrepareDrawConstants, system constants, bindless tables
//     encode   EncodePreparedDraw (render pass begin, dynamic state, setPipeline/setBuffer/draw*)
//     resolve  IssueCopy and everything under it that is not another phase's own scope
//     rt       render-target cache Update (EDRAM ownership transfers, RT creation)
//     wait     blocked in frame-slot, sync command-buffer or pipeline waits (not CPU work)
//     encw_*   blocked inside DeferredRenderEncoder::Drain waiting for the 'GPU Encode' worker, by caller: resolve =
//              under IssueCopy (kResolve scope), draw = under EncodePreparedDraw (kEncode), barrier = Barrier() cross-pass
//              drain, other = any other Drain (EndRenderEncoder/release/Sync outside those). Excluded from every other phase.
//     pm4      PM4 packet dispatch not covered by another scope (base CommandProcessor::ExecutePacket, via
//              rex::graphics::g_packet_scope_hook; includes type-3 handlers without their own scope)
//     draw_misc IssueDraw body not covered by another scope (draw-state decode, shader lookup / analysis, setup)
//     swap     IssueSwap after the row is emitted (frame-boundary work)
//     enc_publish DeferredRenderEncoder::PublishChunk (queue push + worker notify)
//   draws, resolves  IssueDraw / IssueCopy calls
//   pipe_miss        pipeline-state objects requested that were not in the host cache (created or queued)
//   tex_loads        textures loaded or uploaded (texture cache loaded_texture_data_count delta)
//   passes           render encoders begun for draws
//   max_draw_ns      longest single IssueDraw call (includes any queue flush it triggered)
//   max_flush_ns     longest single prepared-draw-queue flush
//   enc_busy_ns      GPU Encode worker thread: ns spent in Replay() since the previous swap (wall, includes preemption)
#include <mach/mach.h>
#include <mach/mach_time.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <time.h>

#include "rex/cvar.h"
#include "rex/graphics/swap_hook.h"

namespace rex::graphics::metal::cpphase {

enum Phase : int {
  kParse = 0,
  kShmem,
  kTex,
  kPipe,
  kVtx,
  kConst,
  kEncode,
  kResolve,
  kRt,
  kWait,
  kEncWaitResolve,
  kEncWaitDraw,
  kEncWaitBarrier,
  kEncWaitOther,
  kPm4,
  kDrawMisc,
  kSwap,
  kEncPublish,
  kPhaseCount
};

inline const char* PhaseName(int p) {
  static const char* const n[kPhaseCount] = {"parse", "shmem", "tex",     "pipe", "vtx",
                                             "const", "encode", "resolve", "rt",   "wait", "encw_resolve", "encw_draw", "encw_barrier", "encw_other", "pm4", "draw_misc", "swap", "enc_publish"};
  return n[p];
}

inline bool Enabled() {
  static const bool on = [] {
    const char* e = std::getenv("REX_METAL_CP_PHASE_TIMERS");
    if (e && e[0] == '1') return true;
    return rex::cvar::Query<bool>("metal_cp_phase_timers");
  }();
  return on;
}

inline std::atomic<uint64_t> g_enc_busy_ticks{0};
inline thread_local bool t_drain_barrier = false;

struct Counters {
  uint64_t draws = 0, resolves = 0, pipe_miss = 0, tex_loads = 0, passes = 0, max_draw_ns = 0, max_flush_ns = 0;
};

struct ThreadState {
  uint64_t ns[kPhaseCount] = {};
  uint32_t calls[kPhaseCount] = {};
  int stack[24] = {};
  uint64_t started[24] = {};
  int depth = 0;
  Counters c;
};

inline ThreadState& State() {
  thread_local ThreadState s;
  return s;
}

inline uint64_t Now() { return mach_absolute_time(); }

inline double TicksToNs() {
  static const double scale = [] {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return double(tb.numer) / double(tb.denom);
  }();
  return scale;
}

inline void Begin(int phase) {
  ThreadState& s = State();
  const uint64_t now = Now();
  if (s.depth > 0 && s.depth <= 24) s.ns[s.stack[s.depth - 1]] += now - s.started[s.depth - 1];
  if (s.depth < 24) {
    s.stack[s.depth] = phase;
    s.started[s.depth] = now;
  }
  ++s.depth;
  ++s.calls[phase];
}

inline void End() {
  ThreadState& s = State();
  if (s.depth <= 0) return;
  --s.depth;
  const uint64_t now = Now();
  if (s.depth < 24) s.ns[s.stack[s.depth]] += now - s.started[s.depth];
  if (s.depth > 0 && s.depth <= 24) s.started[s.depth - 1] = now;
}

// Ticks (not ns) are accumulated; converted when a row is written.
struct Scope {
  bool on;
  explicit Scope(int phase) : on(Enabled()) {
    if (on) Begin(phase);
  }
  ~Scope() {
    if (on) End();
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

// A scope the caller turns on only for part of a function (e.g. only when a pipeline is missing).
struct ScopeIf {
  bool on;
  ScopeIf(int phase, bool a) : on(a && Enabled()) {
    if (on) Begin(phase);
  }
  ~ScopeIf() {
    if (on) End();
  }
  ScopeIf(const ScopeIf&) = delete;
  ScopeIf& operator=(const ScopeIf&) = delete;
};

// Phase for a Drain wait, from the innermost open scope on this thread.
inline int DrainPhase() {
  if (t_drain_barrier) return kEncWaitBarrier;
  ThreadState& s = State();
  const int top = (s.depth > 0 && s.depth <= 24) ? s.stack[s.depth - 1] : -1;
  if (top == kResolve) return kEncWaitResolve;
  if (top == kEncode) return kEncWaitDraw;
  return kEncWaitOther;
}

struct DrainScope {
  bool on;
  DrainScope() : on(Enabled()) {
    if (on) Begin(DrainPhase());
  }
  ~DrainScope() {
    if (on) End();
  }
  DrainScope(const DrainScope&) = delete;
  DrainScope& operator=(const DrainScope&) = delete;
};

// Times one call (IssueDraw, queue flush) for the max-single-call counters.
struct MaxTimer {
  uint64_t* dest;
  uint64_t t0;
  MaxTimer(uint64_t* d, bool a) : dest(a ? d : nullptr), t0(a ? Now() : 0) {}
  ~MaxTimer() {
    if (dest) {
      const uint64_t dt = Now() - t0;
      if (dt > *dest) *dest = dt;
    }
  }
  MaxTimer(const MaxTimer&) = delete;
  MaxTimer& operator=(const MaxTimer&) = delete;
};

inline void Count(uint64_t Counters::*field) {
  if (Enabled()) ++(State().c.*field);
}

class Writer {
 public:
  static Writer& Get() {
    static Writer w;
    return w;
  }
  void Push(std::string&& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) Open();
    if (!file_) return;
    pending_.push_back(std::move(line));
  }

 private:
  void Open() {
    opened_ = true;
    std::string path;
    if (const char* o = std::getenv("REX_METAL_CP_PHASE_OUT")) {
      path = o;
    } else {
      const char* p = std::getenv("REX_BO2ZM_PERF_LOG");
      if (!p) p = std::getenv("BO2ZM_PERF_LOG");
      std::string perf = p ? p : "";
      const size_t slash = perf.rfind('/');
      path = (slash == std::string::npos ? std::string(".") : perf.substr(0, slash)) + "/cp_phases.tsv";
    }
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return;
    std::fputs("# cp_phases 1: per-swap command-processor phase times, see metal_cp_phase.h\n", file_);
    std::fputs("frame\tcpu_ns", file_);
    for (int p = 0; p < kPhaseCount; ++p) std::fprintf(file_, "\t%s_ns", PhaseName(p));
    for (int p = 0; p < kPhaseCount; ++p) std::fprintf(file_, "\t%s_n", PhaseName(p));
    std::fputs("\tdraws\tresolves\tpipe_miss\ttex_loads\tpasses\tmax_draw_ns\tmax_flush_ns\tenc_busy_ns\n", file_);
    std::fflush(file_);
    thread_ = std::thread([this] { Run(); });
    thread_.detach();
  }
  void Run() {
    for (;;) {
      std::deque<std::string> chunk;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(1));
        chunk.swap(pending_);
      }
      for (const std::string& l : chunk) std::fputs(l.c_str(), file_);
      std::fflush(file_);
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> pending_;
  std::FILE* file_ = nullptr;
  bool opened_ = false;
  std::thread thread_;
};

// Called on the command-processor thread at every swap. `cpu_ns` is the thread CPU time since the previous swap,
// `tex_loads_total` the texture cache's cumulative loaded_texture_data_count.
inline void PacketHook(bool begin) {
  if (begin) {
    Begin(kPm4);
  } else {
    End();
  }
}

inline void OnSwap(uint64_t cpu_ns, uint64_t tex_loads_total) {
  if (!Enabled()) return;
  rex::graphics::g_packet_scope_hook = &PacketHook;
  static uint64_t frame = 0;
  static uint64_t last_tex_loads = 0;
  ThreadState& s = State();
  const uint64_t now = Now();
  // Charge the open scope up to the swap boundary (a swap inside a scope is not expected, but stay consistent).
  if (s.depth > 0 && s.depth <= 24) {
    s.ns[s.stack[s.depth - 1]] += now - s.started[s.depth - 1];
    s.started[s.depth - 1] = now;
  }
  const double scale = TicksToNs();
  char buf[1024];
  int len = std::snprintf(buf, sizeof(buf), "%llu\t%llu", (unsigned long long)frame++, (unsigned long long)cpu_ns);
  for (int p = 0; p < kPhaseCount; ++p) {
    len += std::snprintf(buf + len, sizeof(buf) - size_t(len), "\t%llu", (unsigned long long)(double(s.ns[p]) * scale));
  }
  for (int p = 0; p < kPhaseCount; ++p) {
    len += std::snprintf(buf + len, sizeof(buf) - size_t(len), "\t%u", s.calls[p]);
  }
  const uint64_t loads = tex_loads_total >= last_tex_loads ? tex_loads_total - last_tex_loads : 0;
  last_tex_loads = tex_loads_total;
  len += std::snprintf(buf + len, sizeof(buf) - size_t(len), "\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
                       (unsigned long long)s.c.draws, (unsigned long long)s.c.resolves,
                       (unsigned long long)s.c.pipe_miss, (unsigned long long)loads, (unsigned long long)s.c.passes,
                       (unsigned long long)(double(s.c.max_draw_ns) * scale),
                       (unsigned long long)(double(s.c.max_flush_ns) * scale),
                       (unsigned long long)(double(g_enc_busy_ticks.exchange(0, std::memory_order_relaxed)) * scale));
  Writer::Get().Push(std::string(buf, size_t(len)));
  for (int p = 0; p < kPhaseCount; ++p) {
    s.ns[p] = 0;
    s.calls[p] = 0;
  }
  s.c = Counters{};
}

}  // namespace rex::graphics::metal::cpphase
