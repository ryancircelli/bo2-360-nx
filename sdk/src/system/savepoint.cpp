/**
 * @file        system/savepoint.cpp
 * @brief       Save points S1/S2: guest thread quiesce + guest memory dump
 */

#include <rex/system/savepoint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <execinfo.h>
#include <exception>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <typeinfo>
#include <vector>

#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/runtime.h>
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

[[noreturn]] void SavepointTerminate() {
  std::fprintf(stderr, "[savepoint-s3] terminate on host thread 0x%X\n", thread::current_thread_id());
  void* frames[24];
  int count = backtrace(frames, 24);
  backtrace_symbols_fd(frames, count, STDERR_FILENO);
  std::abort();
}

void SavepointAbortSignal(int sig) {
  char buf[96];
  int n = std::snprintf(buf, sizeof buf, "[savepoint-s3] signal %d on host thread 0x%X\n", sig,
                        thread::current_thread_id());
  ::write(STDERR_FILENO, buf, n);
  void* frames[32];
  int count = backtrace(frames, 32);
  backtrace_symbols_fd(frames, count, STDERR_FILENO);
  ::signal(sig, SIG_DFL);
  ::raise(sig);
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
  mach_port_t port = 0;
  arm_thread_state64_t gp{};
  arm_neon_state64_t neon{};
  // Blocked inside a BSD syscall / Mach trap at capture (pc just after `svc #0x80`, thread waiting).
  bool in_syscall = false;
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
  std::shared_ptr<util::ObjectTable::RewindState> object_table;
  std::vector<std::pair<memory::BaseHeap*, memory::BaseHeap::RewindState>> heaps;
  std::vector<object_ref<XObject>> objects_at_rewind;
  IGraphicsSystem* graphics = nullptr;
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

mach_port_t MachPort(XThread* thread) {
  return pthread_mach_thread_np(reinterpret_cast<pthread_t>(thread->thread()->native_handle()));
}

// A thread suspended while blocked in the kernel still owns that kernel wait: thread_set_state alone
// is overwritten when the wait finishes (x0/x1/cpsr, pc-4 on ERESTART), and the wait may never finish.
// Restore is two-phase: thread_abort the wait, point the thread at a user-space spin stub, let it run
// there, re-suspend it (now with no kernel wait in flight) and only then install the captured state.
// The leading nop absorbs a pc-4 restart adjustment applied to the stub.
extern "C" void rex_savepoint_spin_stub();
__asm__(
    ".text\n"
    ".p2align 2\n"
    "  nop\n"
    ".globl _rex_savepoint_spin_stub\n"
    "_rex_savepoint_spin_stub:\n"
    "  mov w9, #1\n"
    "  stlr w9, [x19]\n"
    "1: yield\n"
    "  b 1b\n");

bool AtSyscall(mach_port_t port, const arm_thread_state64_t& gp) {
  uint64_t pc = arm_thread_state64_get_pc(gp);
  if (pc < 4) return false;
  uint32_t insn = 0;
  std::memcpy(&insn, reinterpret_cast<const void*>(pc - 4), 4);
  if (insn != 0xD4001001u) return false;  // svc #0x80
  thread_basic_info_data_t info{};
  mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
  if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) != KERN_SUCCESS)
    return false;
  return info.run_state == TH_STATE_WAITING;
}

// Thread is Mach-suspended on entry and on success. Returns false if it never reached the stub.
bool DetachFromKernelWait(mach_port_t port, const arm_thread_state64_t& base) {
  alignas(8) static std::atomic<uint32_t> flags[512];
  static std::atomic<uint32_t> next{0};
  auto& flag = flags[next.fetch_add(1) % 512];
  flag.store(0);
  arm_thread_state64_t st = base;
  arm_thread_state64_set_pc_fptr(st, reinterpret_cast<void*>(&rex_savepoint_spin_stub));
  st.__x[19] = reinterpret_cast<uint64_t>(&flag);
  thread_abort(port);
  if (thread_set_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&st),
                       ARM_THREAD_STATE64_COUNT) != KERN_SUCCESS)
    return false;
  thread_resume(port);
  bool ok = false;
  for (int i = 0; i < 20000 && !(ok = flag.load(std::memory_order_acquire)); ++i) usleep(50);
  thread_suspend(port);
  return ok;
}

// Mach suspension is synchronous and does not change the guest-visible suspend count.
std::vector<object_ref<XThread>> ParkThreads(KernelState* ks) {
  std::vector<object_ref<XThread>> parked;
  for (auto& t : ks->object_table()->GetObjectsByType<XThread>()) {
    // A thread with several handles is listed once per handle; suspend it only once.
    if (std::any_of(parked.begin(), parked.end(), [&](const auto& p) { return p.get() == t.get(); }))
      continue;
    if (t->is_running() && !XThread::IsInThread(t.get()) &&
        thread_suspend(MachPort(t.get())) == KERN_SUCCESS) {
      parked.push_back(t);
    }
  }
  return parked;
}
}  // namespace

SavepointStats CaptureInProcessSavepoint() {
  std::set_terminate(SavepointTerminate);
  ::signal(SIGABRT, SavepointAbortSignal);
  SavepointStats st;
  auto* ks = kernel_state();
  auto* mem = ks->memory();
  auto t0 = Clock::now();
  auto parked = ParkThreads(ks);
  auto t1 = Clock::now();
  g_snap = InProcess{};
  g_snap.object_table = ks->object_table()->CaptureRewindState();
  if (auto* runtime = rex::Runtime::instance()) {
    g_snap.graphics = runtime->graphics_system();
    if (g_snap.graphics) g_snap.graphics->CaptureSavepoint();
  }
  const uint32_t bases[] = {0x00000000, 0x40000000, 0x80000000, 0x90000000};
  for (uint32_t b : bases) {
    if (auto* heap = mem->LookupHeap(b)) {
      g_snap.heaps.push_back({heap, heap->CaptureRewindState()});
      CollectRegions(heap, 0, mem->virtual_membase(), g_snap.regions);
    }
  }
  auto* physical = mem->GetPhysicalHeap();
  g_snap.heaps.push_back({physical, physical->CaptureRewindState()});
  CollectRegions(physical, 1, mem->physical_membase(), g_snap.regions);
  for (auto& r : g_snap.regions) st.bytes += r.size;
  st.regions = uint32_t(g_snap.regions.size());
  for (auto& t : parked) g_snap.all_parked.push_back(t.get());
  for (auto& t : parked) {
    ThreadSnap ts;
    ts.thread = t;
    ts.port = MachPort(t.get());
    mach_msg_type_number_t gp_count = ARM_THREAD_STATE64_COUNT;
    mach_msg_type_number_t neon_count = ARM_NEON_STATE64_COUNT;
    if (thread_get_state(ts.port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&ts.gp),
                         &gp_count) != KERN_SUCCESS ||
        thread_get_state(ts.port, ARM_NEON_STATE64, reinterpret_cast<thread_state_t>(&ts.neon),
                         &neon_count) != KERN_SUCCESS) {
      continue;
    }
    ts.in_syscall = AtSyscall(ts.port, ts.gp);
    uint64_t sp = arm_thread_state64_get_sp(ts.gp);
    uint64_t top = reinterpret_cast<uint64_t>(
        pthread_get_stackaddr_np(reinterpret_cast<pthread_t>(t->thread()->native_handle())));
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
    thread_resume(MachPort(t.get()));
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
  const bool restore_objects = std::getenv("REX_SAVEPOINT_SKIP_OBJECTS") == nullptr;
  if (restore_objects) {
    g_snap.objects_at_rewind = ks->object_table()->GetAllObjects();
    ks->object_table()->Rewind(g_snap.object_table);
  } else {
    std::fprintf(stderr, "[savepoint-s3] skipping kernel object restore\n");
  }
  const bool restore_memory = std::getenv("REX_SAVEPOINT_SKIP_MEMORY") == nullptr;
  if (!restore_memory) std::fprintf(stderr, "[savepoint-s3] skipping guest memory restore\n");
  uint32_t recommitted = 0, recommit_failed = 0;
  for (auto& r : g_snap.regions) {
    if (!restore_memory) break;
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
  if (restore_memory) {
    for (const auto& [heap, heap_state] : g_snap.heaps) {
      heap->Rewind(heap_state);
    }
    if (g_snap.graphics) g_snap.graphics->RewindSavepoint();
  }
  st.regions = uint32_t(g_snap.regions.size());
  REXSYS_INFO("[savepoint-s3] rewind: recommitted {} regions, {} failed", recommitted, recommit_failed);
  // Threads that exist now but did not at capture time cannot be rewound; counted via parked.
  uint32_t restored = 0, not_parked = 0, detach_failed = 0, in_syscall = 0;
  const bool two_phase = std::getenv("REX_SAVEPOINT_ONE_PHASE") == nullptr;
  const char* sysmode = std::getenv("REX_SAVEPOINT_SYSCALL");
  const bool syscall_eintr = sysmode && std::strcmp(sysmode, "eintr") == 0;
  for (auto& ts : g_snap.threads) {
    if (std::find(parked.begin(), parked.end(), ts.thread.get()) == parked.end()) {
      ++not_parked;
      continue;
    }
    auto td = Clock::now();
    bool detached = !two_phase || DetachFromKernelWait(ts.port, ts.gp);
    if (!detached) ++detach_failed;
    std::fprintf(stderr, "[savepoint-s3] thread %u port %u syscall %d x16 %lld detach %s %.2f ms\n",
                 restored + not_parked, ts.port, int(ts.in_syscall), (long long)ts.gp.__x[16],
                 detached ? "ok" : "FAILED", Ms(td, Clock::now()));
    std::memcpy(reinterpret_cast<void*>(ts.stack_lo), ts.stack.data(), ts.stack.size());
    if (ts.ctx) std::memcpy(ts.ctx, ts.ctx_data.data(), ts.ctx_data.size());
    arm_thread_state64_t gp = ts.gp;
    if (ts.in_syscall) {
      ++in_syscall;
      if (syscall_eintr) {
        // Report the captured wait as interrupted: BSD -> -1/EINTR via carry, Mach trap -> KERN_ABORTED.
        if (int64_t(gp.__x[16]) > 0) { gp.__x[0] = 4; gp.__cpsr |= (1u << 29); }
        else gp.__x[0] = 14;
      } else {
        // Re-execute the captured `svc` with its captured arguments, as the kernel does for ERESTART.
        arm_thread_state64_set_pc_fptr(gp, reinterpret_cast<void*>(arm_thread_state64_get_pc(gp) - 4));
      }
    }
    if (thread_set_state(ts.port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&gp),
                         ARM_THREAD_STATE64_COUNT) != KERN_SUCCESS ||
        thread_set_state(ts.port, ARM_NEON_STATE64, reinterpret_cast<thread_state_t>(&ts.neon),
                         ARM_NEON_STATE64_COUNT) != KERN_SUCCESS) {
      ++not_parked;
      continue;
    }
    ++restored;
  }
  st.threads_parked = restored;
  std::fprintf(stderr,
               "[savepoint-s3] restored %u threads, %u not parked, %u captured in a syscall (%s), %u detach "
               "failures (%s)\n",
               restored, not_parked, in_syscall, syscall_eintr ? "eintr" : "restart", detach_failed,
               two_phase ? "two-phase" : "one-phase");
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
    thread_resume(MachPort(t.get()));
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
std::atomic<IGraphicsSystem*> g_graphics_hook{nullptr};
std::atomic<void (*)(IGraphicsSystem*)> g_graphics_capture{nullptr};
std::atomic<void (*)(IGraphicsSystem*)> g_graphics_rewind{nullptr};

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

void SetGraphicsSavepointHooks(IGraphicsSystem* graphics,
                               void (*capture)(IGraphicsSystem*),
                               void (*rewind)(IGraphicsSystem*)) {
  g_graphics_capture.store(capture, std::memory_order_release);
  g_graphics_rewind.store(rewind, std::memory_order_release);
  g_graphics_hook.store(graphics, std::memory_order_release);
}

void ClearGraphicsSavepointHooks(IGraphicsSystem* graphics) {
  IGraphicsSystem* expected = graphics;
  g_graphics_hook.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void IGraphicsSystem::CaptureSavepoint() {
  if (g_graphics_hook.load(std::memory_order_acquire) == this) {
    if (auto* capture = g_graphics_capture.load(std::memory_order_acquire)) capture(this);
  }
}

void IGraphicsSystem::RewindSavepoint() {
  if (g_graphics_hook.load(std::memory_order_acquire) == this) {
    if (auto* rewind = g_graphics_rewind.load(std::memory_order_acquire)) rewind(this);
  }
}

void RequestSavepoint(const std::string& path) {
  std::lock_guard<std::mutex> l(g_mutex);
  g_request_path = path;
  g_requested.store(true, std::memory_order_release);
}

void NoteSavepointSwap() {
  g_frame.fetch_add(1, std::memory_order_relaxed);
}

void ServiceSavepointRequest() {
  int frame = g_frame.load(std::memory_order_relaxed);
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
