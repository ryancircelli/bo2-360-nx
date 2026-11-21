/**
 * @file        system/savepoint.cpp
 * @brief       Save points S1/S2: guest thread quiesce + guest memory dump
 */

#include <rex/system/savepoint.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_INT32(savepoint_at_frame, -1, "Test",
                     "Take a guest-memory savepoint (savepoint_path) at this swap count; -1 = off");
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
  if (!g_cvar_done) {
    int at = REXCVAR_GET(savepoint_at_frame);
    if (at >= 0 && frame >= at && !REXCVAR_GET(savepoint_path).empty()) {
      g_cvar_done = true;
      Run(REXCVAR_GET(savepoint_path));
    }
  }
}

}  // namespace rex::system
