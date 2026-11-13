// See bo2zm_perf_log.h and docs/mac/perf-log-format.md.
//
// Which renderer numbers go in each column:
//   cpu_ms           the ring thread's time reading and executing packets since the previous presented frame (the
//                    native renderer records and submits on that thread), minus its waits: WAIT_REG_MEM waits for the
//                    game, present_wait_ms and compile_ms. The ring thread is the one that submits, as the format asks.
//   gpu_ms           the span of the timestamp queries of the frame's submissions (the "real GPU per Swap" of the C2
//                    report), collected when each submission's fence is waited on.
//   present_wait_ms  the waits of the C2 "present (ms per call)" report: the fence of the previous output, the SDK's
//                    part of RefreshGuestOutput (acquiring the swapchain image and presenting it, where the vsync
//                    waits), plus the ring's waits for a submission slot still in flight.
//   compile_ms       pipelines created on the ring thread that were not in the driver's cache (with
//                    bo2zm_native_async_pipelines those go to the compile thread and are not counted: the ring does
//                    not wait for them).
//   draws            draws read from the ring.
#include "bo2zm_perf_log.h"
#include <rex/graphics/swap_hook.h>

#include "bo2zm_cheats.h"

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

REXCVAR_DEFINE_STRING(bo2zm_perf_log, "", "BO2",
                      "Test helper: write a per-frame performance log (docs/mac/perf-log-format.md) to this file, "
                      "for tools/mac/perf_summary.py. Empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#ifndef BO2ZM_BUILD_ID
#define BO2ZM_BUILD_ID "dev"
#endif

namespace bo2zm::perf_log {
namespace {
// The renderer line of the header; InstallSwapHook changes it for the emulated GPU plugins.
const char* g_renderer =
#if defined(__APPLE__)
    "native-moltenvk";
#else
    "native-vulkan";
#endif

using Clock = std::chrono::steady_clock;

// t_ms counts from here: static initialization runs before main, close enough to the process start for a log that
// only compares times within itself.
const Clock::time_point g_start = Clock::now();

double MsNow() {
  return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
}

// A frame whose GPU time is not known yet.
struct Row {
  uint64_t frame = 0;
  double t_ms = 0.0, frame_ms = -1.0, cpu_ms = 0.0, present_wait_ms = 0.0, compile_ms = 0.0, enc_wait_ms = 0.0;
  uint64_t draws = 0;
  uint64_t first_job = 0, last_job = 0;  // its submissions: (first_job, last_job]
  uint64_t jobs_left = 0;
  uint64_t gpu_ns = 0;
  bool gpu_measured = false;
  uint32_t age = 0;  // frames presented since
};

// A frame waits at most this many later frames for its GPU time; then it is written with "-". The renderer keeps
// two or three submissions in flight, so a few frames is always enough unless the timestamps are missing.
constexpr uint32_t kMaxAge = 8;
constexpr double kHitchMs = 100.0;

class Log {
 public:
  void OnPresent(const Counters& c) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    const double t = MsNow();
    if (!opened_ && !Open(c)) {
      return;
    }
    Row row;
    row.frame = frames_++;
    row.t_ms = t;
    const double ns_compile = double(compile_ns_);
    compile_ns_ = 0;
    if (have_previous_) {
      row.frame_ms = t - previous_t_;
      const double working = double(c.ns_working - previous_.ns_working);
      const double guest = double(c.ns_waiting_guest - previous_.ns_waiting_guest);
      const double wait = double(c.ns_present_wait - previous_.ns_present_wait);
      const double cpu = working - guest - wait - ns_compile;
      row.cpu_ms = cpu > 0.0 ? cpu / 1e6 : 0.0;
      row.present_wait_ms = wait / 1e6;
      row.enc_wait_ms = double(c.ns_encode_wait - previous_.ns_encode_wait) / 1e6;
      row.draws = c.draws - previous_.draws;
      row.first_job = previous_.jobs_submitted;
    } else {
      // The first frame: what came before the log started is not this frame's. Its own submission is the last one.
      row.first_job = c.jobs_submitted ? c.jobs_submitted - 1 : 0;
    }
    row.compile_ms = ns_compile / 1e6;
    row.last_job = c.jobs_submitted;
    row.jobs_left = row.last_job - row.first_job;
    // Submissions of this frame that already finished (before the frame was presented).
    row.gpu_ns = early_gpu_ns_;
    row.gpu_measured = early_measured_;
    row.jobs_left -= std::min(row.jobs_left, early_jobs_);
    early_gpu_ns_ = early_jobs_ = 0;
    early_measured_ = false;
    previous_ = c;
    previous_t_ = t;
    have_previous_ = true;

    if (!loading_done_ && bo2zm::cheats::InMatch()) {
      loading_done_ = true;
      events_.emplace_back(t, "loading_done");
    }
    if (row.frame_ms > kHitchMs) {
      events_.emplace_back(t, fmt::format("hitch\tms={:.1f}", row.frame_ms));
    }
    rows_.push_back(row);
    for (Row& r : rows_) {
      ++r.age;
    }
    WriteReady(false);
  }

  void NoteGpuJob(uint64_t order, uint64_t ns, bool measured) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_ || closed_) {
      return;
    }
    if (rows_.empty() || order > rows_.back().last_job) {
      // A submission of the frame being built, finished before its present.
      early_gpu_ns_ += measured ? ns : 0;
      early_measured_ |= measured;
      ++early_jobs_;
      return;
    }
    for (Row& r : rows_) {
      if (order > r.first_job && order <= r.last_job) {
        r.gpu_ns += measured ? ns : 0;
        r.gpu_measured |= measured;
        if (r.jobs_left) {
          --r.jobs_left;
        }
        break;
      }
    }
  }

  void NoteCompile(uint64_t ns, uint64_t key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    compile_ns_ += ns;
    events_.emplace_back(MsNow(), fmt::format("compile\tkind=pso\tms={:.3f}\tkey={:016x}", double(ns) / 1e6, key));
  }

  void Event(std::string_view name, std::string_view fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    std::string text(name);
    if (!fields.empty()) {
      text += '\t';
      text += fields;
    }
    events_.emplace_back(MsNow(), std::move(text));
  }

  void SetMeta(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
      return;
    }
    for (auto& [k, v] : meta_) {
      if (k == key) {
        v = value;
        return;
      }
    }
    meta_.emplace_back(std::string(key), std::string(value));
  }

  void Close() {
    std::thread writer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
      if (opened_) {
        WriteReady(true);
        fmt::format_to(std::back_inserter(buffer_), "# end\tframes={}\tduration_ms={:.3f}\n", frames_, MsNow());
      }
      stop_ = true;
      writer = std::move(writer_);
    }
    wake_.notify_all();
    if (writer.joinable()) {
      writer.join();  // it writes what is left and closes the file
    }
  }

 private:
  // With mutex_. The file and the header, at the first presented frame.
  bool Open(const Counters& c) {
    const std::string& path = REXCVAR_GET(bo2zm_perf_log);
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
      REXLOG_WARN("[perf log] could not create {}: the log is off", path);
      closed_ = true;
      return false;
    }
    opened_ = true;
    buffer_ += "# bo2perf 1\n";
    auto meta = [this](std::string_view key, std::string_view value) {
      if (!value.empty()) {
        fmt::format_to(std::back_inserter(buffer_), "# {}\t{}\n", key, value);
      }
    };
    meta("build", BO2ZM_BUILD_ID);
    meta("renderer", g_renderer);
    for (const auto& [k, v] : meta_) {
      meta(k, v);
    }
    meta("os", OsName());
    if (c.width && c.height) {
      meta("resolution", fmt::format("{}x{}", c.width, c.height));
    }
    // tools/mac/boot_test.sh exports the route's name and player count; a run by hand has neither.
    if (const char* players = std::getenv("BO2ZM_PLAYERS")) {
      meta("players", players);
    }
    if (const char* route = std::getenv("BO2ZM_ROUTE")) {
      meta("route", route);
    }
    buffer_ += "frame\tt_ms\tframe_ms\tcpu_ms\tgpu_ms\tpresent_wait_ms\tcompile_ms\tdraws\tenc_wait_ms\n";
    writer_ = std::thread([this] { Writer(); });
    REXLOG_INFO("[perf log] writing {}", path);
    return true;
  }

  static std::string OsName() {
#if defined(__APPLE__)
    char version[64] = {};
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) == 0) {
      return std::string("macOS ") + version;
    }
    return "macOS";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__SWITCH__)
    return "Horizon";
#else
    return "Linux";
#endif
  }

  // With mutex_. Formats the frames whose GPU time is known (all of them when closing), each after the events that
  // came before it, so the file stays in time order.
  void WriteReady(bool all) {
    while (!rows_.empty()) {
      const Row& r = rows_.front();
      if (!all && r.jobs_left && r.age <= kMaxAge) {
        break;
      }
      WriteEvents(r.t_ms);
      const bool gpu_known = r.gpu_measured && !r.jobs_left;
      fmt::format_to(std::back_inserter(buffer_), "{}\t{:.3f}\t", r.frame, r.t_ms);
      if (r.frame_ms < 0.0) {
        buffer_ += "-\t-\t";
      } else {
        fmt::format_to(std::back_inserter(buffer_), "{:.3f}\t{:.3f}\t", r.frame_ms, r.cpu_ms);
      }
      if (gpu_known) {
        fmt::format_to(std::back_inserter(buffer_), "{:.3f}\t", double(r.gpu_ns) / 1e6);
      } else {
        buffer_ += "-\t";
      }
      fmt::format_to(std::back_inserter(buffer_), "{:.3f}\t{:.3f}\t{}\t{:.3f}\n", r.present_wait_ms, r.compile_ms, r.draws,
                     r.enc_wait_ms);
      rows_.pop_front();
    }
    if (all) {
      WriteEvents(MsNow());
    }
  }

  void WriteEvents(double until_ms) {
    size_t n = 0;
    for (; n < events_.size() && events_[n].first <= until_ms; ++n) {
      fmt::format_to(std::back_inserter(buffer_), "#event\t{:.3f}\t{}\n", events_[n].first, events_[n].second);
    }
    events_.erase(events_.begin(), events_.begin() + ptrdiff_t(n));
  }

  // Once a second: the buffer to the file, outside the lock.
  void Writer() {
    rex::thread::set_current_thread_name("BO2 perf log");
    std::string chunk;
    for (;;) {
      bool stop;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; });
        chunk.swap(buffer_);
        stop = stop_;
      }
      if (!chunk.empty()) {
        std::fwrite(chunk.data(), 1, chunk.size(), file_);
        std::fflush(file_);
        chunk.clear();
      }
      if (stop) {
        std::fclose(file_);
        file_ = nullptr;
        return;
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread writer_;
  std::FILE* file_ = nullptr;  // the writer's once open
  std::string buffer_;         // with mutex_: lines not written yet
  bool opened_ = false, closed_ = false, stop_ = false;
  std::vector<std::pair<std::string, std::string>> meta_;
  std::deque<Row> rows_;
  std::vector<std::pair<double, std::string>> events_;
  uint64_t frames_ = 0;
  Counters previous_;
  double previous_t_ = 0.0;
  bool have_previous_ = false;
  bool loading_done_ = false;
  uint64_t compile_ns_ = 0;  // this frame's so far
  uint64_t early_gpu_ns_ = 0, early_jobs_ = 0;
  bool early_measured_ = false;
};

Log& TheLog() {
  static Log log;
  return log;
}

}  // namespace

bool Enabled() {
  static const bool on = !REXCVAR_GET(bo2zm_perf_log).empty();
  return on;
}

void OnPresent(const Counters& counters) {
  if (Enabled()) {
    TheLog().OnPresent(counters);
  }
}

void InstallSwapHook(std::string_view renderer) {
  if (!Enabled()) {
    return;
  }
  static std::string name;
  name = std::string(renderer);
  g_renderer = name.c_str();
  rex::graphics::SetGpuJobHook([](uint64_t order, uint64_t ns, bool measured) { NoteGpuJob(order, ns, measured); });
  rex::graphics::SetSwapHook([](uint32_t width, uint32_t height) {
    // Cumulative counters of the GPU plugin (Metal fills them; the Xenos path leaves them at zero).
    auto& st = rex::graphics::GetPerfStats();
    Counters c;
    c.ns_working = st.ns_working.load(std::memory_order_relaxed);
    c.ns_present_wait = st.ns_present_wait.load(std::memory_order_relaxed);
    c.draws = st.draws.load(std::memory_order_relaxed);
    c.jobs_submitted = st.jobs_submitted.load(std::memory_order_relaxed);
    c.ns_encode_wait = st.ns_encode_wait.load(std::memory_order_relaxed);
    c.width = width;
    c.height = height;
    OnPresent(c);
  });
}

void NoteGpuJob(uint64_t order, uint64_t ns, bool measured) {
  if (Enabled()) {
    TheLog().NoteGpuJob(order, ns, measured);
  }
}

void NoteCompile(uint64_t ns, uint64_t key) {
  if (Enabled()) {
    TheLog().NoteCompile(ns, key);
  }
}

void Event(std::string_view name, std::string_view fields) {
  if (Enabled()) {
    TheLog().Event(name, fields);
  }
}

void SetMeta(std::string_view key, std::string_view value) {
  if (Enabled()) {
    TheLog().SetMeta(key, value);
  }
}

void Close() {
  if (Enabled()) {
    TheLog().Close();
  }
}

}  // namespace bo2zm::perf_log
