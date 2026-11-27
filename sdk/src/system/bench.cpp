#include <rex/system/bench.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <rex/graphics/swap_hook.h>
#include <rex/input/script/script_events.h>
#include <rex/system/savepoint.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <unistd.h>
// Private libproc flavors (xnu bsd/sys/proc_info_private.h), checked on macOS 27: thread ids, and per-thread counts
// {u64 levels; per perf level (0 = P cores) instr, cycles, user, system (mach ticks), energy}.
#ifndef PROC_PIDLISTTHREADIDS
#define PROC_PIDLISTTHREADIDS 28
#endif
#ifndef PROC_PIDTHREADCOUNTS
#define PROC_PIDTHREADCOUNTS 34
#endif
#endif

REXCVAR_DEFINE_INT32(bench_frames, 0, "Bench",
                     "Deterministic benchmark: measure exactly this many presented frames (0 = off). Use with "
                     "clock_frame_lock_hz so every frame covers the same guest time. See bench_out.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bench_warmup_frames, 0, "Bench",
                     "Frames between the window being armed and its start.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bench_arm_frame, -1, "Bench",
                     "Arm the window at this presented frame number; -1 = the game arms it (its own scene start).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bench_arm_on_log, "", "Bench",
                      "Arm the window when a log line containing this text first appears (a game's own scene-start "
                      "marker, e.g. a level file opening); needs no game code. Empty = off.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bench_inject_m_instr, 0, "Bench",
                     "Test of the bench itself: burn this many million instructions of real work on the presenting "
                     "thread every frame (a known cost to detect). 0 = off.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bench_spin_gap_bits, 0, "Bench",
                     "Instructions a thread runs between two waits or yields count as spin (a wait loop's own check) "
                     "when fewer than 2^bits; more is work. 0 = only what runs inside the waits is spin.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bench_rounds, 1, "Bench",
                     "Measure the window this many times from one saved state: the game is saved (in memory) where the "
                     "first window would start and rewound there after each window, so every round plays the same "
                     "scene. Each round logs its own [bench] json; the last one is followed by [bench] done.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bench_round_warmup_frames, 30, "Bench",
                     "With bench_rounds > 1: frames after the save or rewind before each round's window.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bench_out, "", "Bench", "Write the window's result here as JSON (empty = log only).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::bench {
namespace {

struct ThreadCounts {
  std::string name;
  uint64_t instr[2] = {}, cycles[2] = {}, cpu_ns[2] = {};  // [0] performance cores, [1] efficiency cores
  rex::thread::SpinCounts spin;                               // rex::thread::SpinScope counts
};
struct Snapshot {
  double wall_s = 0;
  uint64_t instr = 0, cycles = 0, pinstr = 0, cpu_ns = 0, pcpu_ns = 0;
  uint64_t draws = 0;  // draws the GPU plugin read from the ring (0 where it does not count them)
  std::map<uint64_t, ThreadCounts> threads;
};

std::atomic<uint64_t> g_frame{0};
std::atomic<int64_t> g_arm_frame{-1};  // frame the window starts at
std::atomic<uint64_t> g_gpu_ns{0};
std::vector<uint64_t> g_frame_gpu_ns;  // GPU time finished between two frames, in the window (swap thread only)
uint64_t g_last_gpu = 0;
std::atomic<int> g_state{0};  // 0 waiting, 1 in the window, 2 done
Snapshot g_start;
uint64_t g_start_stalls = 0, g_start_gpu = 0;
std::string g_reason;
int g_round = 0;  // rounds finished
bool g_saved = false;
uint64_t g_saved_clock = 0;  // guest ticks at the save
std::atomic<int> g_pending{0};  // 1 save, 2 rewind: for ServiceRounds

double MachToNs() {
#if defined(__APPLE__)
  static const double r = [] {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return double(tb.numer) / tb.denom;
  }();
  return r;
#else
  return 1.0;
#endif
}

Snapshot Take() {
  Snapshot s;
  s.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  s.draws = rex::graphics::GetPerfStats().draws.load(std::memory_order_relaxed);
#if defined(__APPLE__)
  const int pid = getpid();
  rusage_info_v6 r{};
  if (proc_pid_rusage(pid, RUSAGE_INFO_V6, reinterpret_cast<rusage_info_t*>(&r)) == 0) {
    s.instr = r.ri_instructions;
    s.cycles = r.ri_cycles;
    s.pinstr = r.ri_pinstructions;
    s.cpu_ns = uint64_t((r.ri_user_time + r.ri_system_time) * MachToNs());
    s.pcpu_ns = uint64_t((r.ri_user_ptime + r.ri_system_ptime) * MachToNs());
  }
  std::vector<uint64_t> ids(1024);
  const int n = proc_pidinfo(pid, PROC_PIDLISTTHREADIDS, 0, ids.data(), int(ids.size() * sizeof(uint64_t)));
  for (int i = 0; i < n / int(sizeof(uint64_t)); ++i) {
    uint64_t buf[32] = {};
    if (proc_pidinfo(pid, PROC_PIDTHREADCOUNTS, ids[i], buf, sizeof(buf)) < 56) continue;
    ThreadCounts t;
    const int levels = std::min<int>(int(buf[0] & 0xffff), 2);
    for (int lv = 0; lv < levels; ++lv) {
      const uint64_t* b = buf + 1 + lv * 5;
      t.instr[lv] = b[0];
      t.cycles[lv] = b[1];
      t.cpu_ns[lv] = uint64_t((b[2] + b[3]) * MachToNs());
    }
    proc_threadinfo ti{};
    if (proc_pidinfo(pid, PROC_PIDTHREADID64INFO, ids[i], &ti, sizeof(ti)) > 0) t.name = ti.pth_name;
    s.threads[ids[i]] = std::move(t);
  }
  rex::thread::ForEachSpinCount(
      [](uint64_t tid, const rex::thread::SpinCounts& c, void* ctx) {
        auto& m = static_cast<Snapshot*>(ctx)->threads;
        if (auto it = m.find(tid); it != m.end()) it->second.spin = c;
      },
      &s);
#endif
  return s;
}

std::string JsonEscape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '"' || c == '\\') out += '\\';
    if (uint8_t(c) >= 0x20) out += c;
  }
  return out;
}

void Finish(uint64_t frames) {
  const Snapshot end = Take();
  const double gpu_ms = double(g_gpu_ns.load() - g_start_gpu) / 1e6;
  const uint64_t stalls = chrono::Clock::frame_lock_stalls() - g_start_stalls;
  std::string th;
  uint64_t spin_total = 0, gap_total = 0;
  const int gap_bits = std::clamp(REXCVAR_GET(bench_spin_gap_bits), 0, rex::thread::kSpinGapBuckets - 1);
  for (const auto& [tid, e] : end.threads) {
    auto it = g_start.threads.find(tid);
    ThreadCounts b = it != g_start.threads.end() ? it->second : ThreadCounts{};  // a thread born in the window: 0
    const uint64_t instr = e.instr[0] + e.instr[1] - b.instr[0] - b.instr[1];
    const uint64_t cyc = e.cycles[0] + e.cycles[1] - b.cycles[0] - b.cycles[1];
    const uint64_t ns = e.cpu_ns[0] + e.cpu_ns[1] - b.cpu_ns[0] - b.cpu_ns[1];
    const uint64_t pns = e.cpu_ns[0] - b.cpu_ns[0];
    const uint64_t spin = e.spin.instr - b.spin.instr;
    spin_total += spin;
    uint64_t gap_spin = 0;
    std::string gsum, gn;
    int last = 0;  // highest bucket in use, so the lists stop there
    for (int k = 0; k < rex::thread::kSpinGapBuckets; ++k) {
      if (e.spin.gap_n[k] != b.spin.gap_n[k]) last = k;
      if (k <= gap_bits) gap_spin += e.spin.gap_sum[k] - b.spin.gap_sum[k];
    }
    for (int k = 0; k <= last; ++k) {
      gsum += fmt::format("{}{}", k ? "," : "", e.spin.gap_sum[k] - b.spin.gap_sum[k]);
      gn += fmt::format("{}{}", k ? "," : "", e.spin.gap_n[k] - b.spin.gap_n[k]);
    }
    gap_total += gap_spin;
    if (!instr && !ns) continue;
    th += fmt::format("{}{{\"tid\":{},\"name\":\"{}\",\"instr\":{},\"spin_instr\":{},\"gap_spin\":{},\"cycles\":{},"
                      "\"cpu_ms\":{:.3f},\"p_cpu_ms\":{:.3f},\"gap_sum\":[{}],\"gap_n\":[{}]}}",
                      th.empty() ? "" : ",\n  ", tid, JsonEscape(e.name), instr, spin, gap_spin, cyc, ns / 1e6, pns / 1e6,
                      gsum, gn);
  }
  const double wall = end.wall_s - g_start.wall_s;
  // Median GPU time of a frame: a frame that shared the GPU with another process reads long, the median does not move.
  std::vector<uint64_t> fg = g_frame_gpu_ns;
  std::sort(fg.begin(), fg.end());
  const double gpu_p50 = fg.empty() ? 0 : fg[fg.size() / 2] / 1e6, gpu_p25 = fg.empty() ? 0 : fg[fg.size() / 4] / 1e6;
  const uint64_t total_instr = end.instr - g_start.instr;
  const uint64_t work = total_instr - std::min(total_instr, spin_total + gap_total);
  const std::string json = fmt::format(
      "{{\"frames\":{},\"wall_s\":{:.3f},\"fps\":{:.2f},\"frame_lock_hz\":{},\"stall_frames\":{},\"reason\":\"{}\",\n"
      "\"instr\":{},\"spin_instr\":{},\"gap_spin\":{},\"spin_gap_bits\":{},\"work\":{},\"cycles\":{},\"p_instr\":{},\"cpu_ms\":{:.3f},"
      "\"p_cpu_ms\":{:.3f},\"gpu_ms\":{:.3f},\"gpu_frame_ms_p50\":{:.4f},\"gpu_frame_ms_p25\":{:.4f},\n"
      "\"draws\":{},\"round\":{},\"rounds\":{},\n\"threads\":[\n  {}]}}\n",
      frames, wall, frames / wall, REXCVAR_GET(clock_frame_lock_hz), stalls, JsonEscape(g_reason),
      total_instr, spin_total, gap_total, gap_bits, work, end.cycles - g_start.cycles, end.pinstr - g_start.pinstr,
      (end.cpu_ns - g_start.cpu_ns) / 1e6, (end.pcpu_ns - g_start.pcpu_ns) / 1e6, gpu_ms, gpu_p50, gpu_p25, end.draws - g_start.draws, g_round + 1, std::max(1, REXCVAR_GET(bench_rounds)), th);
  const std::string& path = REXCVAR_GET(bench_out);
  if (!path.empty()) {
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fwrite(json.data(), 1, json.size(), f);
      std::fclose(f);
    } else {
      REXLOG_WARN("[bench] cannot write {}", path);
    }
  }
  std::string line = json;
  for (char& c : line) if (c == '\n') c = ' ';
  REXLOG_INFO("[bench] json {}", line);  // the whole result in the log too, for runners that cannot pass a path
  const bool last = g_round + 1 >= REXCVAR_GET(bench_rounds);
  REXLOG_INFO("[bench] {}: {} frames in {:.1f} s ({:.1f} fps), {:.1f} M work instructions/frame ({:.1f} M spin of "
              "{:.1f} M), {:.2f} GPU ms/frame (median {:.2f}), {:.0f} draws/frame, {} stall frames -> {}",
              last ? "done" : fmt::format("round {}", g_round + 1), frames, wall, frames / wall, double(work) / frames / 1e6, double(spin_total + gap_total) / frames / 1e6,
              double(total_instr) / frames / 1e6, gpu_ms / frames, gpu_p50, double(end.draws - g_start.draws) / frames, stalls, path.empty() ? "log" : path);
}

}  // namespace

bool Enabled() {
  static const bool on = [] {
    const bool b = REXCVAR_GET(bench_frames) > 0;
    if (b) rex::thread::EnableSpinCount();
    return b;
  }();
  return on;
}

void Arm(const char* reason) {
  if (!Enabled()) return;
  int64_t expected = -1;
  const int64_t at = int64_t(g_frame.load()) + std::max(0, REXCVAR_GET(bench_warmup_frames));
  if (g_arm_frame.compare_exchange_strong(expected, at)) {
    g_reason = reason ? reason : "";
    // A fast-forwarded boot (guest_time_scale, e.g. 2) ends here: the window and what leads to it run at 1x.
    if (chrono::Clock::guest_time_scalar() != 1.0) {
      REXLOG_INFO("[bench] guest_time_scale {} -> 1 for the window", chrono::Clock::guest_time_scalar());
      chrono::Clock::set_guest_time_scalar(1.0);
    }
    REXLOG_INFO("[bench] armed ({}): window of {} frames starts at frame {}", g_reason, REXCVAR_GET(bench_frames), at);
  }
}

void AddGpuNs(uint64_t ns) {
  g_gpu_ns.fetch_add(ns, std::memory_order_relaxed);
}

void OnFrame() {
  chrono::Clock::OnGuestFrame();
  if (!Enabled()) return;
  // Only inside the window: loading screens present far more frames/s and would crawl. Measured ~15 instructions per iteration.
  if (static const int inject = REXCVAR_GET(bench_inject_m_instr); inject > 0 && g_state.load() == 1) {
    volatile uint64_t sink = 0;
    for (uint64_t i = 0, n = uint64_t(inject) * 66000; i < n; ++i) sink = sink + i;
  }
  const uint64_t frame = g_frame.fetch_add(1) + 1;
  if (static const int at = REXCVAR_GET(bench_arm_frame); at >= 0 && frame == uint64_t(at)) {
    Arm("bench_arm_frame");
  }
  static const std::string on_log = REXCVAR_GET(bench_arm_on_log);
  if (!on_log.empty()) {
    static const bool watching = (input::script::WatchLogText(on_log), true);
    (void)watching;
    if (input::script::ScriptEventCount(input::script::LogEventName(on_log)) > 0) Arm("bench_arm_on_log");
  }
  const int64_t start = g_arm_frame.load();
  const int state = g_state.load();
  if (state == 0 && start >= 0 && int64_t(frame) >= start && REXCVAR_GET(bench_rounds) > 1 && !g_saved) {
    g_saved = true;
    g_pending = 1;  // ServiceRounds saves between batches; the window waits for it
    g_arm_frame = INT64_MAX;
  } else if (state == 0 && start >= 0 && int64_t(frame) >= start) {
    g_start = Take();
    g_start_stalls = chrono::Clock::frame_lock_stalls();
    g_start_gpu = g_last_gpu = g_gpu_ns.load();
    g_frame_gpu_ns.reserve(size_t(std::max(0, REXCVAR_GET(bench_frames))));
    g_state = 1;
    REXLOG_INFO("[bench] window start at frame {}", frame);
  } else if (state == 1) {
    const uint64_t gpu = g_gpu_ns.load();
    g_frame_gpu_ns.push_back(gpu - g_last_gpu);
    g_last_gpu = gpu;
  }
  if (state == 1 && int64_t(frame) >= start + REXCVAR_GET(bench_frames)) {
    g_state = 2;
    Finish(frame - uint64_t(start));
    if (++g_round < REXCVAR_GET(bench_rounds)) {
      g_arm_frame = INT64_MAX;
      g_state = 0;
      g_pending = 2;
    }
  }
}

void ServiceRounds() {
  const int op = g_pending.exchange(0);
  if (!op) return;
  const uint64_t frame = g_frame.load();
  if (op == 1) {
    g_saved_clock = chrono::Clock::frame_lock_base();
    const auto st = rex::system::CaptureInProcessSavepoint();
    if (!st.error.empty()) {
      REXLOG_WARN("[bench] save for rounds failed ({}): one round only", st.error);
      REXCVAR_SET(bench_rounds, 1);
    } else {
      REXLOG_INFO("[bench] saved at frame {} for {} rounds ({} MB)", frame, REXCVAR_GET(bench_rounds), st.bytes >> 20);
    }
  } else {
    const auto st = rex::system::RewindToInProcessSavepoint();
    if (!st.error.empty()) {
      REXLOG_WARN("[bench] rewind failed ({}): stopping after {} rounds", st.error, g_round);
      REXLOG_INFO("[bench] done: rewind failed");
      return;
    }
    rex::thread::SpinCountRewound();
    if (chrono::Clock::frame_locked()) chrono::Clock::RewindFrameLock(g_saved_clock);  // pads, audio, timers replay
    g_frame_gpu_ns.clear();
    REXLOG_INFO("[bench] rewound for round {} (restore {:.1f} ms)", g_round + 1, st.write_ms);
  }
  g_arm_frame = int64_t(frame) + std::max(0, REXCVAR_GET(bench_round_warmup_frames));
}

}  // namespace rex::bench
