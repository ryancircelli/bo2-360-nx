// See bo2zm_jobs.h. The periodic report of BO2's job system (bo2zm_jobs_report_s seconds, 0 = off).

#include "bo2zm_jobs.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

REXCVAR_DEFINE_INT32(bo2zm_jobs_report_s, 0, "BO2",
                     "Diagnostics: every this many seconds, log the state of the game's job system and what each "
                     "game thread did in it (picks, waits, yields, host blocks). 0 = off")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::jobs {
namespace {

std::mutex g_mutex;
std::vector<PerThread*> g_threads;                      // never freed: game threads live as long as the game
std::map<uint32_t, uint64_t> g_queues;               // queue -> jobs pushed in the window
std::map<uint32_t, uint64_t> g_types;               // job type descriptor -> jobs pushed in the window
std::map<uint32_t, uint64_t> g_returned;           // job type descriptor -> jobs put back in the window
std::map<uint64_t, std::pair<uint64_t, uint64_t>> g_waits;  // (who << 32 | lr) -> (calls, ns) in the window
std::map<uint32_t, std::string> g_queue_names;     // queue -> first thread that pushed to it
std::map<uint64_t, uint64_t> g_dependencies;        // (counter << 32 | job type) -> unmet picks in the window
thread_local PerThread* t_thread = nullptr;
std::once_flag g_boot;

uint64_t Take(std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); }

std::string DescribeQueue(const uint8_t* base, uint32_t queue) {
  return fmt::format("{:08X} jobs {} mask {:02X} dest {:08X} open {} pending {}", queue,
                     Read32(base, queue + kQueueCount), Read32(base, queue + kQueueMask),
                     Read32(base, queue + kQueueTarget), Read32(base, queue + kQueueOpen),
                     Read32(base, queue + kQueuePending));
}

void Report(const uint8_t* base, double seconds) {
  // The job system's own state, read now.
  const uint32_t num = Read32(base, kNumSlots);
  const uint32_t slots = Read32(base, kSlots);
  REXLOG_INFO("[jobs] state: queued {} running {} | {} worker slots (hw mask {:02X}) | workers waiting {} | pause {} "
              "quit {} | helper slots in use {} | default queue {} | preferred queue {}",
              Read32(base, kInQueue), Read32(base, kInRunning), num, Read32(base, kMaskThreads),
              Read32(base, kWorkersWaiting), Read32(base, kPause), base[kExit],
              Read32(base, kHelpsInOngoing), DescribeQueue(base, kDefaultQueue),
              DescribeQueue(base, kQueuePreferred));
  for (uint32_t i = 0; i < num && i < 8 && slots; ++i) {
    const uint32_t r = slots + i * kSzSlot;
    std::string queues;
    for (int k = 0; k < kQueuesPerSlot; ++k) {
      const uint32_t c = Read32(base, r + kSlotQueues + 4 * k);
      if (c) {
        queues += fmt::format(" {:08X}", c);
      }
    }
    REXLOG_INFO("[jobs]   slot {} at {:08X}: hw mask {:02X}, thread {:08X}, own queue {} jobs, registered:{}", i,
                r, Read32(base, r + kSlotMask), Read32(base, r + 8),
                Read32(base, r + kSlotOwn + kQueueCount), queues);
  }

  std::lock_guard lock(g_mutex);
  // Per thread, per second.
  auto ps = [&](uint64_t v) { return double(v) / seconds; };
  auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / seconds; };  // ms per second
  for (PerThread* h : g_threads) {
    const uint64_t choose = Take(h->choose), dependent = Take(h->choose_dependent), v0 = Take(h->choose_empty[0]), v1 = Take(h->choose_empty[1]),
                   v2 = Take(h->choose_empty[2]), yielded = Take(h->yielded),
                   wait = Take(h->wait_worker), no_sleep = Take(h->wait_no_sleep),
                   ns_wait = Take(h->ns_wait_worker), polls = Take(h->polls),
                   ready = Take(h->polls_ready), queued = Take(h->queued), returned = Take(h->returned), helps = Take(h->helps),
                   ns_helps = Take(h->ns_helps), type = Take(h->waits_type), ns_type = Take(h->ns_waits_type),
                   stalls = Take(h->stalls), warned = Take(h->stalls_warned),
                   ns_stalls = Take(h->ns_stalls);
    if (!(choose | yielded | wait | polls | queued | returned | helps | type | stalls)) {
      continue;
    }
    REXLOG_INFO("[jobs] {} [t{}]: picks {:.0f}/s (empty: {:.0f} idle, {:.0f} raced, {:.0f} elsewhere; {:.0f} dependency "
                "not met) | yields {:.0f}/s | worker waits {:.0f}/s ({:.0f} without sleeping, {:.1f} ms/s) | frame "
                "polls {:.0f}/s ({:.0f} ready) | pushes {:.0f}/s, put back {:.0f}/s | helps {:.0f}/s ({:.1f} ms/s) | "
                "type waits {:.0f}/s ({:.1f} ms/s) | host blocks {:.0f}/s ({:.0f} notified, {:.1f} ms/s)",
                h->label, h->id, ps(choose), ps(v0), ps(v1), ps(v2), ps(dependent), ps(yielded), ps(wait), ps(no_sleep),
                ms(ns_wait), ps(polls), ps(ready), ps(queued), ps(returned), ps(helps), ms(ns_helps), ps(type),
                ms(ns_type), ps(stalls), ps(warned), ms(ns_stalls));
  }
  static const char* const kNamesWarnings[kNumWarnings] = {"job", "release list", "job done", "frame event", "ring",
                                                                "lock 6 left"};
  std::string warnings;
  for (int m = 0; m < kNumWarnings; ++m) {
    warnings += fmt::format("{}{} {:.0f}/s", m ? ", " : "", kNamesWarnings[m], ps(Take(g_warnings[m])));
  }
  REXLOG_INFO("[jobs]   notifications: {}", warnings);
  static const char* const kNamesRooms[kNumRooms] = {"frame poll", "done poll", "ring poll", "after release",
                                                         "worker", "type wait", "group wait", "server",
                                                         "dependency", "lock 6", "put back in a row",
                                                         "culling jobs"};
  std::string rooms;
  for (int k = 0; k < kNumRooms; ++k) {
    const uint64_t times = Take(g_rooms[k].times), exhausted = Take(g_rooms[k].exhausted),
                   ns = Take(g_rooms[k].ns);
    if (times) {
      rooms += fmt::format(" | {} {:.0f}/s ({:.0f} timed out, {:.1f} ms/s)", kNamesRooms[k], ps(times),
                            ps(exhausted), ms(ns));
    }
  }
  REXLOG_INFO("[jobs]   sleeps:{}", rooms);
  // Where the jobs went.
  std::vector<std::pair<uint64_t, uint32_t>> order;
  for (auto& [queue, n] : g_queues) {
    order.emplace_back(n, queue);
  }
  std::sort(order.rbegin(), order.rend());
  for (size_t i = 0; i < order.size() && i < 12; ++i) {
    REXLOG_INFO("[jobs]   queue {}: {:.0f} pushes/s (first from {})", DescribeQueue(base, order[i].second),
                ps(order[i].first), g_queue_names[order[i].second]);
  }
  g_queues.clear();
  order.clear();
  for (auto& [type, n] : g_types) {
    order.emplace_back(n, type);
  }
  std::sort(order.rbegin(), order.rend());
  for (size_t i = 0; i < order.size() && i < 16; ++i) {
    const uint32_t t = order[i].second;
    REXLOG_INFO("[jobs]   type {:08X} (function {:08X}): {:.0f} pushes/s, now {} queued {} running", t,
                Read32(base, t + 8), ps(order[i].first), Read32(base, t + 16), Read32(base, t + 20));
  }
  g_types.clear();
  order.clear();
  for (auto& [type, n] : g_returned) {
    order.emplace_back(n, type);
  }
  std::sort(order.rbegin(), order.rend());
  for (size_t i = 0; i < order.size() && i < 8; ++i) {
    const uint32_t t = order[i].second;
    REXLOG_INFO("[jobs]   type {:08X} (function {:08X}): {:.0f} put back/s", t, Read32(base, t + 8),
                ps(order[i].first));
  }
  g_returned.clear();
  // Jobs picked before their dependency was met.
  std::vector<std::pair<uint64_t, uint64_t>> deps;
  for (auto& [key, n] : g_dependencies) {
    deps.emplace_back(n, key);
  }
  std::sort(deps.rbegin(), deps.rend());
  for (size_t i = 0; i < deps.size() && i < 8; ++i) {
    const uint32_t c = uint32_t(deps[i].second >> 32), type = uint32_t(deps[i].second);
    REXLOG_INFO("[jobs]   dependency counter {:08X} (now {}) of type {:08X} (function "
                "{:08X}): {:.0f} unmet picks/s", c, Read32(base, c), type, Read32(base, type + 8), ps(deps[i].first));
  }
  g_dependencies.clear();
  // Who waits for what.
  std::vector<std::pair<uint64_t, uint64_t>> waits;
  for (auto& [key, v] : g_waits) {
    waits.emplace_back(v.second, key);
  }
  std::sort(waits.rbegin(), waits.rend());
  for (size_t i = 0; i < waits.size() && i < 16; ++i) {
    const uint64_t key = waits[i].second;
    const auto& v = g_waits[key];
    REXLOG_INFO("[jobs]   wait sub_{:08X} from {:08X}: {:.0f}/s, {:.2f} ms/s", uint32_t(key >> 32),
                uint32_t(key), ps(v.first), ms(v.second));
  }
  g_waits.clear();
}

}  // namespace

bool ReportActive() {
  static const bool active = REXCVAR_GET(bo2zm_jobs_report_s) > 0;
  return active;
}

PerThread& JobThread() {
  if (!t_thread) [[unlikely]] {
    auto* h = new PerThread();
    // The guest thread's host thread carries its name ("Backend (F800003C)"); Thread::GetCurrentThread() is
    // another object on Win32, without it.
    auto* xthread = rex::system::XThread::GetCurrentThread();
    rex::thread::Thread* actual = xthread && xthread->thread() ? xthread->thread() : rex::thread::Thread::GetCurrentThread();
    h->label = actual ? (new std::string(actual->name()))->c_str() : "?";
    h->id = actual ? actual->system_id() : 0;
    std::lock_guard lock(g_mutex);
    g_threads.push_back(h);
    t_thread = h;
  }
  return *t_thread;
}

void Boot(uint8_t* base) {
  std::call_once(g_boot, [base] {
    const int32_t interval = REXCVAR_GET(bo2zm_jobs_report_s);
    REXLOG_INFO("[jobs] report every {} s", interval);
    // Never freed and never detached: on Horizon std::thread::detach() throws and the game closes.
    new std::thread([base, interval] {
      rex::thread::set_current_thread_name("BO2 job report");
      auto earlier = std::chrono::steady_clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        const auto now = std::chrono::steady_clock::now();
        Report(base, std::chrono::duration<double>(now - earlier).count());
        earlier = now;
      }
    });
  });
}

int ClassifyEmpty(const uint8_t* base, uint32_t slot) {
  if (Read32(base, kInQueue) == 0) {
    return 0;
  }
  uint32_t visible = Read32(base, slot + kSlotOwn + kQueueCount);
  bool preferred = false;
  for (int k = 0; k < kQueuesPerSlot; ++k) {
    const uint32_t c = Read32(base, slot + kSlotQueues + 4 * k);
    if (c) {
      visible += Read32(base, c + kQueueCount);
      preferred |= c == kQueuePreferred;
    }
  }
  if (!preferred) {
    visible += Read32(base, kQueuePreferred + kQueueCount);
  }
  return visible ? 1 : 2;
}

void AnnotateQueued(const uint8_t* base, uint32_t job, uint32_t queue) {
  const uint32_t type = Read32(base, job + kJobType);
  const char* label = JobThread().label;  // before the lock: JobThread() takes it on a thread's first call
  std::lock_guard lock(g_mutex);
  ++g_queues[queue];
  ++g_types[type];
  if (!g_queue_names.count(queue)) {
    g_queue_names[queue] = label;
  }
}

void AnnotateReturned(uint32_t type) {
  std::lock_guard lock(g_mutex);
  ++g_returned[type];
}

void AnnotateDependency(uint32_t counter, uint32_t type) {
  std::lock_guard lock(g_mutex);
  ++g_dependencies[uint64_t(counter) << 32 | type];
}

void AnnotateWait(uint32_t who, uint32_t lr, uint64_t ns) {
  std::lock_guard lock(g_mutex);
  auto& v = g_waits[uint64_t(who) << 32 | lr];
  v.first += 1;
  v.second += ns;
}

}  // namespace bo2zm::jobs
