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
#include <mach/mach.h>
#include <pthread.h>
#include <map>
#endif

REXCVAR_DEFINE_STRING(bo2zm_perf_log, "", "BO2",
                      "Test helper: write a per-frame performance log (docs/mac/perf-log-format.md) to this file, "
                      "for tools/mac/perf_summary.py. Empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_perf_counters, false, "BO2",
                    "Test helper: append main_ms, server_ms, worker_ms, tex_new, tex_ms, upload_bytes columns to the "
                    "perf log (see bo2zm_perf_log.cpp)")
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

#if defined(__APPLE__)
// bo2zm_perf_counters: CPU time (user+system, thread_info) of the game's threads, summed per role by host thread name:
//   main_ms    threads named "Main XThread*": CPU time since the previous presented frame
//   server_ms  threads named "Server*"
//   worker_ms  sum over threads named "Worker*" or "Job*" (the job workers; Audio Worker excluded)
// Sampled on the ring thread at each present; the interval is the same as frame_ms.
struct ThreadSampler {
  struct T { std::string name; uint64_t ns = 0; };
  std::map<mach_port_t, T> known;
  double main_ms = 0, server_ms = 0, worker_ms = 0;
  std::string all_names;
  static uint64_t Cpu(mach_port_t t) {
    thread_basic_info_data_t i; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&i, &n) != KERN_SUCCESS) return 0;
    return (uint64_t(i.user_time.seconds) + i.system_time.seconds) * 1000000000ull +
           (uint64_t(i.user_time.microseconds) + i.system_time.microseconds) * 1000ull;
  }
  void Sample() {
    thread_act_array_t list; mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return;
    uint64_t m = 0, sv = 0, w = 0;
    for (mach_msg_type_number_t k = 0; k < count; ++k) {
      mach_port_t t = list[k];
      auto it = known.find(t);
      if (it == known.end()) {
        T e; char buf[128] = {};
        if (pthread_t pt = pthread_from_mach_thread_np(t)) pthread_getname_np(pt, buf, sizeof buf);
        e.name = buf; e.ns = Cpu(t);
        all_names += e.name + ";";
        known.emplace(t, e);
      } else {
        const uint64_t now = Cpu(t), d = now - it->second.ns;
        it->second.ns = now;
        const std::string& n = it->second.name;
        if (n.rfind("Main XThread", 0) == 0) m += d;
        else if (n.rfind("Server", 0) == 0) sv += d;
        else if (n.rfind("JQ Worker", 0) == 0) w += d;
      }
      mach_port_deallocate(mach_task_self(), t);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(*list));
    main_ms = m / 1e6; server_ms = sv / 1e6; worker_ms = w / 1e6;
  }
};
#else
struct ThreadSampler { double main_ms = 0, server_ms = 0, worker_ms = 0; std::string all_names; void Sample() {} };
#endif

// A frame whose GPU time is not known yet.
struct Row {
  uint64_t frame = 0;
  double t_ms = 0.0, frame_ms = -1.0, cpu_ms = 0.0, present_wait_ms = 0.0, compile_ms = 0.0;
  uint64_t draws = 0;
  double main_ms = 0, server_ms = 0, worker_ms = 0, tex_ms = 0;
  uint64_t tex_new = 0, upload_bytes = 0;
  uint64_t extra[15] = {};
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
    if (counters_on_ && !have_previous_) {
      threads_.Sample();  // baselines
    }
    if (counters_on_ && frames_ == 1800 && !threads_.all_names.empty()) {
      events_.emplace_back(t, "threads\t" + threads_.all_names);
    }
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
      row.draws = c.draws - previous_.draws;
      row.tex_new = c.tex_new - previous_.tex_new;
      row.tex_ms = double(c.ns_tex - previous_.ns_tex) / 1e6;
      row.upload_bytes = c.upload_bytes - previous_.upload_bytes;
      for (int k = 0; k < 14; ++k) row.extra[k] = c.extra[k] - previous_.extra[k];
      if (counters_on_) {
        threads_.Sample();
        row.main_ms = threads_.main_ms; row.server_ms = threads_.server_ms; row.worker_ms = threads_.worker_ms;
      }
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
    buffer_ += "frame\tt_ms\tframe_ms\tcpu_ms\tgpu_ms\tpresent_wait_ms\tcompile_ms\tdraws";
    if (counters_on_) {
      buffer_ += "\tmain_ms\tserver_ms\tworker_ms\ttex_new\ttex_ms\tupload_bytes\tupload_ms\tupload_ranges\tup_vertex\tup_memexport\tup_index\tup_texture\tup_resolve\tup_other\tdif_cmp\tdif_chg4k\tdif_chg64\tdif_pages16\tdif_same16\tdif_nohist";
    }
    buffer_ += "\n";
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
      fmt::format_to(std::back_inserter(buffer_), "{:.3f}\t{:.3f}\t{}", r.present_wait_ms, r.compile_ms, r.draws);
      if (counters_on_) {
        fmt::format_to(std::back_inserter(buffer_), "\t{:.3f}\t{:.3f}\t{:.3f}\t{}\t{:.3f}\t{}", r.main_ms, r.server_ms,
                       r.worker_ms, r.tex_new, r.tex_ms, r.upload_bytes);
        fmt::format_to(std::back_inserter(buffer_), "\t{:.3f}", double(r.extra[0]) / 1e6);
        for (int k = 1; k < 14; ++k) fmt::format_to(std::back_inserter(buffer_), "\t{}", r.extra[k]);
      }
      buffer_ += '\n';
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
  const bool counters_on_ = REXCVAR_GET(bo2zm_perf_counters);
  ThreadSampler threads_;
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
  if (REXCVAR_GET(bo2zm_perf_counters)) rex::graphics::GetPerfStats().diff_enabled.store(true);
  rex::graphics::SetGpuJobHook([](uint64_t order, uint64_t ns, bool measured) { NoteGpuJob(order, ns, measured); });
  rex::graphics::SetSwapHook([](uint32_t width, uint32_t height) {
    // Cumulative counters of the GPU plugin (Metal fills them; the Xenos path leaves them at zero).
    auto& st = rex::graphics::GetPerfStats();
    Counters c;
    c.ns_working = st.ns_working.load(std::memory_order_relaxed);
    c.ns_present_wait = st.ns_present_wait.load(std::memory_order_relaxed);
    c.draws = st.draws.load(std::memory_order_relaxed);
    c.jobs_submitted = st.jobs_submitted.load(std::memory_order_relaxed);
    c.tex_new = st.tex_new.load(std::memory_order_relaxed);
    c.ns_tex = st.ns_tex.load(std::memory_order_relaxed);
    c.upload_bytes = st.upload_bytes.load(std::memory_order_relaxed);
    c.extra[0] = st.ns_upload.load(std::memory_order_relaxed);
    c.extra[1] = st.upload_runs.load(std::memory_order_relaxed);
    for (int k = 0; k < 6; ++k) c.extra[2 + k] = st.up_cat[k].load(std::memory_order_relaxed);
    c.extra[8] = st.dif_cmp.load(std::memory_order_relaxed);
    c.extra[9] = st.dif_chg4k.load(std::memory_order_relaxed);
    c.extra[10] = st.dif_chg64.load(std::memory_order_relaxed);
    c.extra[11] = st.dif_pages16.load(std::memory_order_relaxed);
    c.extra[12] = st.dif_same16.load(std::memory_order_relaxed);
    c.extra[13] = st.dif_nohist.load(std::memory_order_relaxed);
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
