/**
 * @file        system/savepoint.cpp
 * @brief       Save points S1/S2: guest thread quiesce + guest memory dump
 */

#include <rex/system/savepoint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <typeinfo>
#include <vector>

#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <sys/ucontext.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/thread.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_INT32(savepoint_at_frame, -1, "Test",
                     "Take a guest-memory savepoint (savepoint_path) at this swap count; -1 = off");
REXCVAR_DEFINE_INT32(savepoint_mem_at_frame, -1, "Test",
                     "S3 experiment: capture an in-process savepoint at this swap count; -1 = off");
REXCVAR_DEFINE_INT32(savepoint_rewind_at_frame, -1, "Test",
                     "S3 experiment: rewind to the in-process savepoint at this swap count; -1 = off");
REXCVAR_DEFINE_STRING(savepoint_path, "", "Test", "File the savepoint is written to");

namespace rex::system {

namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

bool WriteAll(int fd, const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  while (size) {
    ssize_t n = ::write(fd, p, size);
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

// Walks `heap` and writes every committed, accessible region read from `membase`.
bool DumpHeap(int fd, memory::BaseHeap* heap, const uint8_t* membase, uint32_t space,
              SavepointStats& st) {
  uint64_t addr = heap->heap_base();
  uint64_t end = uint64_t(heap->heap_base()) + heap->heap_size();
  while (addr < end) {
    memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(addr), &info)) break;
    uint32_t size = info.region_size ? info.region_size : heap->page_size();
    if ((info.state & memory::kMemoryAllocationCommit) && info.protect != memory::kMemoryProtectNoAccess &&
        info.region_size) {
      uint32_t hdr[3] = {space, uint32_t(addr), size};
      if (!WriteAll(fd, hdr, sizeof(hdr)) || !WriteAll(fd, membase + addr, size)) return false;
      ++st.regions;
      st.bytes += size;
    }
    addr += size;
  }
  return true;
}
}  // namespace

SavepointStats SaveGuestMemory(const std::string& path) {
  SavepointStats st;
  auto* ks = kernel_state();
  if (!ks) {
    st.error = "no kernel state";
    return st;
  }
  auto* mem = ks->memory();
  auto t0 = Clock::now();
  // S2: park every other running guest thread.
  std::vector<object_ref<XThread>> parked;
  for (auto& t : ks->object_table()->GetObjectsByType<XThread>()) {
    if (t->is_guest_thread() && t->is_running() && !XThread::IsInThread(t.get())) {
      // Host-level suspend only: XThread::Suspend also bumps the guest-visible suspend_count,
      // which perturbs guest waits (first run froze the title after resume).
      uint32_t host_count = 0;
      if (t->thread()->Suspend(&host_count)) parked.push_back(t);
    }
  }
  auto t1 = Clock::now();
  st.threads_parked = uint32_t(parked.size());
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  bool ok = fd >= 0 && WriteAll(fd, "REXSP1\0\0", 8);
  if (ok) {
    const uint32_t bases[] = {0x00000000, 0x40000000, 0x80000000, 0x90000000};
    for (uint32_t b : bases) {
      if (auto* heap = mem->LookupHeap(b)) ok = ok && DumpHeap(fd, heap, mem->virtual_membase(), 0, st);
    }
    ok = ok && DumpHeap(fd, mem->GetPhysicalHeap(), mem->physical_membase(), 1, st);
    uint32_t endmark[3] = {0xFFFFFFFFu, 0, 0};
    ok = ok && WriteAll(fd, endmark, sizeof(endmark));
  }
  if (fd >= 0) ::close(fd);
  auto t2 = Clock::now();
  for (auto& t : parked) {
    uint32_t host_count = 0;
    t->thread()->Resume(&host_count);
  }
  auto t3 = Clock::now();
  st.park_ms = Ms(t0, t1);
  st.write_ms = Ms(t1, t2);
  st.resume_ms = Ms(t2, t3);
  st.ok = ok;
  if (!ok) st.error = "write failed: " + path;
  return st;
}

namespace {
struct MemRegion {
  uint32_t space, base, size, protect;
  std::vector<uint8_t> data;
};
struct ThreadSnap {
  object_ref<XThread> thread;
  // Interrupted register state saved by the suspend signal handler (parked on its alternate stack).
  std::vector<uint8_t> mctx;
  uint64_t stack_lo = 0, stack_hi = 0;
  std::vector<uint8_t> stack;
  // PPCContext (guest registers) lives in the heap-allocated ThreadState, not on the host stack.
  void* ctx = nullptr;
  std::vector<uint8_t> ctx_data;
};
struct InProcess {
  bool valid = false;
  std::vector<MemRegion> regions;
  std::vector<ThreadSnap> threads;
  std::vector<XThread*> all_parked;  // every thread parked at capture, snapshotted or not
} g_snap;

bool CollectRegions(memory::BaseHeap* heap, uint32_t space, const uint8_t* membase,
                    std::vector<MemRegion>& out) {
  uint64_t addr = heap->heap_base();
  uint64_t end = uint64_t(heap->heap_base()) + heap->heap_size();
  while (addr < end) {
    memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(addr), &info)) break;
    uint32_t size = info.region_size ? info.region_size : heap->page_size();
    if ((info.state & memory::kMemoryAllocationCommit) &&
        info.protect != memory::kMemoryProtectNoAccess && info.region_size) {
      out.push_back({space, uint32_t(addr), size, info.protect, {membase + addr, membase + addr + size}});
    }
    addr += size;
  }
  return true;
}

// Waits (up to 2 s) until the thread is parked in the suspend signal handler; returns its saved ucontext.
ucontext_t* WaitParkedContext(XThread* t) {
  for (int i = 0; i < 40; ++i) {
    if (void* uc = thread::GetSuspendedContext(t->thread())) return static_cast<ucontext_t*>(uc);
    usleep(1000);
  }
  return nullptr;
}

// Parks guest threads (host level) and returns them.
std::vector<object_ref<XThread>> ParkThreads(KernelState* ks) {
  std::vector<object_ref<XThread>> parked;
  for (auto& t : ks->object_table()->GetObjectsByType<XThread>()) {
    if (t->is_guest_thread() && t->is_running() && !XThread::IsInThread(t.get())) {
      uint32_t c = 0;
      if (t->thread()->Suspend(&c)) parked.push_back(t);
    }
  }
  return parked;
}
}  // namespace

SavepointStats CaptureInProcessSavepoint() {
  SavepointStats st;
  auto* ks = kernel_state();
  auto* mem = ks->memory();
  auto t0 = Clock::now();
  auto parked = ParkThreads(ks);
  auto t1 = Clock::now();
  g_snap = InProcess{};
  const uint32_t bases[] = {0x00000000, 0x40000000, 0x80000000, 0x90000000};
  for (uint32_t b : bases) {
    if (auto* heap = mem->LookupHeap(b)) CollectRegions(heap, 0, mem->virtual_membase(), g_snap.regions);
  }
  CollectRegions(mem->GetPhysicalHeap(), 1, mem->physical_membase(), g_snap.regions);
  for (auto& r : g_snap.regions) st.bytes += r.size;
  st.regions = uint32_t(g_snap.regions.size());
  for (auto& t : parked) g_snap.all_parked.push_back(t.get());
  for (auto& t : parked) {
    ThreadSnap ts;
    ts.thread = t;
    ucontext_t* uc = WaitParkedContext(t.get());
    if (!uc) continue;
    pthread_t pt = reinterpret_cast<pthread_t>(t->thread()->native_handle());
    ts.mctx.assign(reinterpret_cast<uint8_t*>(uc->uc_mcontext),
                   reinterpret_cast<uint8_t*>(uc->uc_mcontext) + sizeof(*uc->uc_mcontext));
    uint64_t sp = arm_thread_state64_get_sp(uc->uc_mcontext->__ss);
    uint64_t top = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(pt));
    if (sp == 0 || sp >= top || top - sp > (64u << 20)) continue;
    ts.stack_lo = sp - 256;  // include the red zone
    ts.stack_hi = top;
    ts.stack.assign(reinterpret_cast<uint8_t*>(ts.stack_lo), reinterpret_cast<uint8_t*>(ts.stack_hi));
    if (auto* tstate = t->thread_state()) {
      ts.ctx = tstate->context();
      ts.ctx_data.assign(reinterpret_cast<uint8_t*>(ts.ctx),
                         reinterpret_cast<uint8_t*>(ts.ctx) + sizeof(PPCContext));
    }
    g_snap.threads.push_back(std::move(ts));
  }
  g_snap.valid = true;
  auto t2 = Clock::now();
  for (auto& t : parked) {
    uint32_t c = 0;
    t->thread()->Resume(&c);
  }
  st.threads_parked = uint32_t(g_snap.threads.size());
  st.park_ms = Ms(t0, t1);
  st.write_ms = Ms(t1, t2);
  st.ok = true;
  return st;
}

SavepointStats RewindToInProcessSavepoint() {
  SavepointStats st;
  if (!g_snap.valid) {
    st.error = "no in-process savepoint";
    return st;
  }
  auto* ks = kernel_state();
  auto* mem = ks->memory();
  auto t0 = Clock::now();
  auto parked = ParkThreads(ks);
  std::fprintf(stderr, "[savepoint-s3] rewind start, %zu parked\n", parked.size());
  auto t1 = Clock::now();
  uint32_t recommitted = 0, recommit_failed = 0;
  for (auto& r : g_snap.regions) {
    uint8_t* base = r.space == 0 ? mem->virtual_membase() : mem->physical_membase();
    // Pages committed at capture may have been decommitted since, or are read-only now (XEX image
    // pages, write-watched pages). Walk the range run by run and make every run committed + writable,
    // memcpy, then put the captured protection back.
    memory::BaseHeap* heap = r.space == 0 ? mem->LookupHeap(r.base) : mem->GetPhysicalHeap();
    std::vector<std::pair<uint32_t, uint32_t>> touched;
    if (heap) {
      uint64_t a = r.base, end = uint64_t(r.base) + r.size;
      bool failed = false;
      while (a < end) {
        memory::HeapAllocationInfo cur{};
        if (!heap->QueryRegionInfo(uint32_t(a), &cur)) { failed = true; break; }
        uint32_t run = cur.region_size ? cur.region_size : heap->page_size();
        uint32_t len = uint32_t(std::min<uint64_t>(run, end - a));
        const uint32_t rw = memory::kMemoryProtectRead | memory::kMemoryProtectWrite;
        if (!(cur.state & memory::kMemoryAllocationCommit)) {
          if (!heap->AllocFixed(uint32_t(a), len, heap->page_size(),
                                memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit, rw)) {
            failed = true; break;
          }
          ++recommitted; touched.push_back({uint32_t(a), len});
          std::fprintf(stderr, "[savepoint-s3] recommit %08X +%X (captured prot %u)\n", uint32_t(a), len, r.protect);
        } else if ((cur.protect & rw) != rw) {
          if (!heap->Protect(uint32_t(a), len, rw)) { failed = true; break; }
          touched.push_back({uint32_t(a), len});
          std::fprintf(stderr, "[savepoint-s3] reprotect %08X +%X (cur prot %u, captured %u)\n", uint32_t(a), len, cur.protect, r.protect);
        }
        a += len;
      }
      if (failed) { ++recommit_failed; continue; }
    }
    std::memcpy(base + r.base, r.data.data(), r.size);
    for (auto& t : touched) heap->Protect(t.first, t.second, r.protect);
    st.bytes += r.size;
  }
  st.regions = uint32_t(g_snap.regions.size());
  REXSYS_INFO("[savepoint-s3] rewind: recommitted {} regions, {} failed", recommitted, recommit_failed);
  // Threads that exist now but did not at capture time cannot be rewound; counted via parked.
  uint32_t restored = 0, not_parked = 0;
  for (auto& ts : g_snap.threads) {
    ucontext_t* uc = WaitParkedContext(ts.thread.get());
    if (!uc) {
      ++not_parked;
      continue;
    }
    // The thread's own stack is untouched while it sits in the handler on its alternate stack, so the
    // whole saved stack, the saved interrupted registers and the PPCContext can be put back.
    std::memcpy(reinterpret_cast<void*>(ts.stack_lo), ts.stack.data(), ts.stack.size());
    std::memcpy(uc->uc_mcontext, ts.mctx.data(), ts.mctx.size());
    if (ts.ctx) std::memcpy(ts.ctx, ts.ctx_data.data(), ts.ctx_data.size());
    ++restored;
  }
  st.threads_parked = restored;
  std::fprintf(stderr, "[savepoint-s3] restored %u threads, %u not parked\n", restored, not_parked);
  auto t2 = Clock::now();
  uint32_t left_parked = 0;
  for (auto& t : parked) {
    // Threads created after the capture have no saved state and their guest memory is gone: keep them
    // suspended for good rather than let them run against rewound memory.
    bool known = std::find(g_snap.all_parked.begin(), g_snap.all_parked.end(), t.get()) !=
                 g_snap.all_parked.end();
    if (!known) {
      ++left_parked;
      continue;
    }
    uint32_t c = 0;
    t->thread()->Resume(&c);
  }
  std::fprintf(stderr, "[savepoint-s3] rewind done, %u threads left parked\n", left_parked);
  REXSYS_INFO("[savepoint-s3] rewind: {} threads created after the capture left suspended", left_parked);
  st.park_ms = Ms(t0, t1);
  st.write_ms = Ms(t1, t2);
  st.ok = true;
  return st;
}

namespace {
std::atomic<bool> g_requested{false};
std::mutex g_mutex;
std::string g_request_path;
std::atomic<int> g_frame{0};
bool g_cvar_done = false;

void Run(const std::string& path) {
  SavepointStats st = SaveGuestMemory(path);
  if (st.ok) {
    REXSYS_INFO(
        "[savepoint] wrote {} ({} MB in {} regions): park {:.1f} ms ({} threads), write {:.1f} ms, "
        "resume {:.1f} ms",
        path, st.bytes >> 20, st.regions, st.park_ms, st.threads_parked, st.write_ms, st.resume_ms);
  } else {
    REXSYS_ERROR("[savepoint] failed: {}", st.error);
  }
}
}  // namespace

void RequestSavepoint(const std::string& path) {
  std::lock_guard<std::mutex> l(g_mutex);
  g_request_path = path;
  g_requested.store(true, std::memory_order_release);
}

void ServiceSavepointRequest() {
  int frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  if (g_requested.load(std::memory_order_acquire)) {
    std::string path;
    {
      std::lock_guard<std::mutex> l(g_mutex);
      path = g_request_path;
      g_requested.store(false, std::memory_order_release);
    }
    Run(path);
    return;
  }
  {
    static bool captured = false, rewound = false;
    int cap = REXCVAR_GET(savepoint_mem_at_frame), rew = REXCVAR_GET(savepoint_rewind_at_frame);
    if (cap >= 0 && !captured && frame >= cap) {
      captured = true;
      auto st = CaptureInProcessSavepoint();
      REXSYS_INFO("[savepoint-s3] captured at frame {}: {} MB, {} regions, {} threads with host state, park {:.1f} ms, copy {:.1f} ms",
                  frame, st.bytes >> 20, st.regions, st.threads_parked, st.park_ms, st.write_ms);
    } else if (rew >= 0 && captured && !rewound && frame >= rew) {
      rewound = true;
      auto st = RewindToInProcessSavepoint();
      REXSYS_INFO("[savepoint-s3] rewound at frame {}: {} MB, {} threads restored, park {:.1f} ms, restore {:.1f} ms (frame counter keeps running; watch the game for continuity)",
                  frame, st.bytes >> 20, st.threads_parked, st.park_ms, st.write_ms);
    }
  }
  if (!g_cvar_done) {
    int at = REXCVAR_GET(savepoint_at_frame);
    if (at >= 0 && frame >= at && !REXCVAR_GET(savepoint_path).empty()) {
      g_cvar_done = true;
      Run(REXCVAR_GET(savepoint_path));
    }
  }
}

}  // namespace rex::system
