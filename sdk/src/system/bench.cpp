#include <rex/system/bench.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

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
REXCVAR_DEFINE_STRING(bench_out, "", "Bench", "Write the window's result here as JSON (empty = log only).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::bench {
namespace {

struct ThreadCounts {
  std::string name;
  uint64_t instr[2] = {}, cycles[2] = {}, cpu_ns[2] = {};  // [0] performance cores, [1] efficiency cores
  uint64_t spin = 0;                                          // instructions in rex::thread::SpinScope
};
struct Snapshot {
  double wall_s = 0;
  uint64_t instr = 0, cycles = 0, pinstr = 0, cpu_ns = 0, pcpu_ns = 0;
  std::map<uint64_t, ThreadCounts> threads;
};

std::atomic<uint64_t> g_frame{0};
std::atomic<int64_t> g_arm_frame{-1};  // frame the window starts at
std::atomic<uint64_t> g_gpu_ns{0};
std::atomic<int> g_state{0};  // 0 waiting, 1 in the window, 2 done
Snapshot g_start;
uint64_t g_start_stalls = 0, g_start_gpu = 0;
std::string g_reason;

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
      [](uint64_t tid, uint64_t instr, void* ctx) {
        auto& m = static_cast<Snapshot*>(ctx)->threads;
        if (auto it = m.find(tid); it != m.end()) it->second.spin = instr;
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
  uint64_t spin_total = 0;
  for (const auto& [tid, e] : end.threads) {
    auto it = g_start.threads.find(tid);
    ThreadCounts b = it != g_start.threads.end() ? it->second : ThreadCounts{};  // a thread born in the window: 0
    const uint64_t instr = e.instr[0] + e.instr[1] - b.instr[0] - b.instr[1];
    const uint64_t cyc = e.cycles[0] + e.cycles[1] - b.cycles[0] - b.cycles[1];
    const uint64_t ns = e.cpu_ns[0] + e.cpu_ns[1] - b.cpu_ns[0] - b.cpu_ns[1];
    const uint64_t pns = e.cpu_ns[0] - b.cpu_ns[0];
    const uint64_t spin = e.spin - b.spin;
    spin_total += spin;
    if (!instr && !ns) continue;
    th += fmt::format("{}{{\"tid\":{},\"name\":\"{}\",\"instr\":{},\"spin_instr\":{},\"cycles\":{},\"cpu_ms\":{:.3f},"
                      "\"p_cpu_ms\":{:.3f}}}",
                      th.empty() ? "" : ",\n  ", tid, JsonEscape(e.name), instr, spin, cyc, ns / 1e6, pns / 1e6);
  }
  const double wall = end.wall_s - g_start.wall_s;
  const std::string json = fmt::format(
      "{{\"frames\":{},\"wall_s\":{:.3f},\"fps\":{:.2f},\"frame_lock_hz\":{},\"stall_frames\":{},\"reason\":\"{}\",\n"
      "\"instr\":{},\"spin_instr\":{},\"cycles\":{},\"p_instr\":{},\"cpu_ms\":{:.3f},\"p_cpu_ms\":{:.3f},\"gpu_ms\":{:.3f},\n"
      "\"threads\":[\n  {}]}}\n",
      frames, wall, frames / wall, REXCVAR_GET(clock_frame_lock_hz), stalls, JsonEscape(g_reason),
      end.instr - g_start.instr, spin_total, end.cycles - g_start.cycles, end.pinstr - g_start.pinstr,
      (end.cpu_ns - g_start.cpu_ns) / 1e6, (end.pcpu_ns - g_start.pcpu_ns) / 1e6, gpu_ms, th);
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
  REXLOG_INFO("[bench] done: {} frames in {:.1f} s ({:.1f} fps), {:.1f} M instructions/frame ({:.1f} M of them spin), {:.2f} GPU ms/frame, "
              "{} stall frames -> {}",
              frames, wall, frames / wall, double(end.instr - g_start.instr) / frames / 1e6, double(spin_total) / frames / 1e6, gpu_ms / frames, stalls,
              path.empty() ? "log" : path);
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
    REXLOG_INFO("[bench] armed ({}): window of {} frames starts at frame {}", g_reason, REXCVAR_GET(bench_frames), at);
  }
}

void AddGpuNs(uint64_t ns) {
  g_gpu_ns.fetch_add(ns, std::memory_order_relaxed);
}

void OnFrame() {
  chrono::Clock::OnGuestFrame();
  if (!Enabled()) return;
  const uint64_t frame = g_frame.fetch_add(1) + 1;
  if (static const int at = REXCVAR_GET(bench_arm_frame); at >= 0 && frame == uint64_t(at)) {
    Arm("bench_arm_frame");
  }
  const int64_t start = g_arm_frame.load();
  const int state = g_state.load();
  if (state == 0 && start >= 0 && int64_t(frame) >= start) {
    g_start = Take();
    g_start_stalls = chrono::Clock::frame_lock_stalls();
    g_start_gpu = g_gpu_ns.load();
    g_state = 1;
    REXLOG_INFO("[bench] window start at frame {}", frame);
  } else if (state == 1 && int64_t(frame) >= start + REXCVAR_GET(bench_frames)) {
    g_state = 2;
    Finish(frame - uint64_t(start));
  }
}

}  // namespace rex::bench
