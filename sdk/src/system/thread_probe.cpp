// Guest thread timeline probe; see rex/system/thread_probe.h. macOS only (elsewhere it stays off).
#include <rex/system/thread_probe.h>

#include <rex/thread.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#endif

namespace rex::thread_probe {

#if defined(__APPLE__)

namespace {

struct Rec {
  uint64_t t;  // mach_absolute_time
  uint32_t id;
  uint8_t kind;  // written last (release); 0 = slot not committed yet
  uint8_t a;
  uint16_t b;
  uint32_t obj;
  uint32_t lr;
};
static_assert(sizeof(Rec) == 24);

constexpr uint64_t kSlots = 1ull << 21;  // 48 MiB ring
Rec* g_ring = nullptr;
std::atomic<uint64_t> g_head{0};
FILE* g_file = nullptr;
FILE* g_names = nullptr;

struct Entry {
  uint32_t id;
  mach_port_t port;
  uint64_t last_cpu_us;
  std::atomic<bool>* waiting;
};
std::mutex g_mutex;
std::vector<Entry> g_threads;

thread_local uint32_t t_id = 0;
thread_local bool t_registered = false;
thread_local std::atomic<bool>* t_waiting = nullptr;  // leaked on purpose: the sampler may read it after exit

uint32_t CurrentId() { return t_id ? t_id : rex::thread::current_thread_id(); }

void Put(uint32_t id, uint8_t kind, uint8_t a, uint16_t b, uint32_t obj, uint32_t lr) {
  const uint64_t slot = g_head.fetch_add(1, std::memory_order_relaxed) & (kSlots - 1);
  Rec& r = g_ring[slot];
  r.t = mach_absolute_time();
  r.id = id;
  r.a = a;
  r.b = b;
  r.obj = obj;
  r.lr = lr;
  reinterpret_cast<std::atomic<uint8_t>&>(r.kind).store(kind, std::memory_order_release);
}

uint64_t CpuUs(mach_port_t port, int* run_state) {
  thread_basic_info_data_t info;
  mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
  if (thread_info(port, THREAD_BASIC_INFO, thread_info_t(&info), &n) != KERN_SUCCESS) {
    *run_state = -1;
    return 0;
  }
  *run_state = info.run_state;
  return uint64_t(info.user_time.seconds + info.system_time.seconds) * 1000000 +
         info.user_time.microseconds + info.system_time.microseconds;
}

void SamplerLoop() {
  pthread_setname_np("ThreadProbe");
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  const uint64_t period = 1000000ull * tb.denom / tb.numer;  // 1 ms in ticks
  uint64_t next = mach_absolute_time();
  for (;;) {
    next += period;
    mach_wait_until(next);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Entry& e : g_threads) {
      if (!e.port) continue;
      int state = 0;
      const uint64_t cpu = CpuUs(e.port, &state);
      if (state < 0) { e.port = 0; continue; }
      const uint64_t d = e.last_cpu_us ? cpu - e.last_cpu_us : 0;
      e.last_cpu_us = cpu;
      Put(e.id, kSample, uint8_t(state), e.waiting->load(std::memory_order_relaxed) ? 1 : 0,
          uint32_t(d), 0);
    }
  }
}

void WriterLoop() {
  pthread_setname_np("ThreadProbeWriter");
  uint64_t tail = 0;
  std::vector<Rec> out;
  out.reserve(65536);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t head = g_head.load(std::memory_order_acquire);
    if (head - tail > kSlots) {  // writer fell behind; skip lost records
      std::fprintf(stderr, "[thread-probe] lost %llu records\n",
                   (unsigned long long)(head - tail - kSlots));
      tail = head - kSlots;
    }
    while (tail < head) {
      Rec& r = g_ring[tail & (kSlots - 1)];
      auto& k = reinterpret_cast<std::atomic<uint8_t>&>(r.kind);
      if (k.load(std::memory_order_acquire) == 0) break;  // not committed yet; next pass
      out.push_back(r);
      k.store(0, std::memory_order_relaxed);
      ++tail;
      if (out.size() == 65536) {
        std::fwrite(out.data(), sizeof(Rec), out.size(), g_file);
        out.clear();
      }
    }
    if (!out.empty()) {
      std::fwrite(out.data(), sizeof(Rec), out.size(), g_file);
      out.clear();
    }
    std::fflush(g_file);
    std::fflush(g_names);
  }
}

bool Init() {
  const char* path = std::getenv("REX_THREAD_PROBE");
  if (!path || !*path) return false;
  g_file = std::fopen(path, "wb");
  g_names = std::fopen((std::string(path) + ".threads").c_str(), "w");
  if (!g_file || !g_names) return false;
  g_ring = static_cast<Rec*>(std::calloc(kSlots, sizeof(Rec)));
  if (!g_ring) return false;
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  // Header: magic, timebase numer/denom, start tick.
  const uint32_t hdr[4] = {0x50524254u /*TBRP*/, tb.numer, tb.denom, 0};
  std::fwrite(hdr, sizeof(hdr), 1, g_file);
  const uint64_t start = mach_absolute_time();
  std::fwrite(&start, sizeof(start), 1, g_file);
  std::fwrite(&start, sizeof(start), 1, g_file);  // pad header to 24 bytes + 8
  std::thread(SamplerLoop).detach();
  std::thread(WriterLoop).detach();
  std::fprintf(stderr, "[thread-probe] writing %s\n", path);
  return true;
}

}  // namespace

bool g_on = Init();

void RegisterCurrent(uint32_t id, const char* name) {
  if (!g_on || t_registered) return;
  t_registered = true;
  t_id = id;
  const mach_port_t port = pthread_mach_thread_np(pthread_self());
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  std::lock_guard<std::mutex> lock(g_mutex);
  t_waiting = new std::atomic<bool>(false);
  g_threads.push_back(Entry{id, port, 0, t_waiting});
  std::fprintf(g_names, "%08X\t%llu\t%s\n", id, (unsigned long long)tid, name ? name : "");
}

void NoteName(uint32_t id, const char* name) {
  if (!g_on) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  std::fprintf(g_names, "%08X\t0\t%s\n", id, name ? name : "");
}

void Emit(uint8_t kind, uint8_t a, uint16_t b, uint32_t obj, uint32_t lr) {
  if (!g_on) return;
  Put(CurrentId(), kind, a, b, obj, lr);
}

void EmitAs(uint32_t id, uint8_t kind, uint8_t a, uint16_t b, uint32_t obj, uint32_t lr) {
  if (!g_on) return;
  Put(id, kind, a, b, obj, lr);
}

void SetWaiting(bool waiting) {
  if (!g_on || !t_waiting) return;
  t_waiting->store(waiting, std::memory_order_relaxed);
}

#else

bool g_on = false;
void RegisterCurrent(uint32_t, const char*) {}
void Emit(uint8_t, uint8_t, uint16_t, uint32_t, uint32_t) {}
void EmitAs(uint32_t, uint8_t, uint8_t, uint16_t, uint32_t, uint32_t) {}
void SetWaiting(bool) {}
void NoteName(uint32_t, const char*) {}

#endif

}  // namespace rex::thread_probe
