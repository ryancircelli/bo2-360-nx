/**
 * @file        system/savepoint.cpp
 * @brief       Save points S1/S2: guest thread quiesce + guest memory dump
 */

#include <rex/system/savepoint.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/xma/decoder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <execinfo.h>
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <sys/mman.h>
#include <exception>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <condition_variable>
#include <memory>
#include <mach/semaphore.h>
#include <list>
#include <functional>
#include <mutex>
#include <rex/thread/mutex.h>
#include <thread>
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
#include <rex/thread/fiber.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/thread_state.h>
#include <rex/chrono/clock.h>
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
  // Host fibers: guest fiber map, and each fiber's object (saved context) and host stack bytes.
  std::unordered_map<uint32_t, FiberInfo> fiber_map;
  struct FiberSnap { rex::thread::Fiber* fiber; std::vector<uint8_t> object, stack; };
  std::vector<FiberSnap> fibers;
  std::vector<XThread*> all_parked;
  audio::XmaDecoder* xma_decoder = nullptr;
  std::shared_ptr<audio::XmaDecoder::RewindState> xma;  // every thread parked at capture, snapshotted or not
  std::shared_ptr<util::ObjectTable::RewindState> object_table;
  std::list<std::function<void()>> dispatch_queue;
  uint32_t dpc_head = 0;
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
std::vector<object_ref<XThread>> ParkThreadsOnce(KernelState* ks);

// A thread suspended while it holds the global critical region (object table, kernel lists) or a
// malloc zone lock deadlocks the capture/rewind that follows (seen in NFSMW: rewind stuck in
// ObjectTable::GetAllObjects). Park, check those locks are free, otherwise resume and retry.
semaphore_t g_probe_sem;
std::atomic<uint64_t> g_probe_req{0}, g_probe_done{0};
void* MallocProbeMain(void*) {
  while (true) {
    semaphore_wait(g_probe_sem);
    uint64_t gen = g_probe_req.load();
    for (size_t n : {16u, 96u, 1024u, 16384u, 200000u, 3u << 20}) {
      void* volatile p = std::malloc(n);
      std::free(p);
    }
    g_probe_done.store(gen);
  }
  return nullptr;
}
void StartMallocProbe() {
  static bool started = false;
  if (started) return;
  started = true;
  semaphore_create(mach_task_self(), &g_probe_sem, SYNC_POLICY_FIFO, 0);
  pthread_t t;
  pthread_create(&t, nullptr, MallocProbeMain, nullptr);
  pthread_detach(t);
}
bool ParkedLocksFree() {
  auto& m = rex::thread::global_critical_region::mutex();
  if (!m.try_lock()) return false;
  m.unlock();
  // Malloc: introspect->zone_locked traps on the xzone allocator (macOS 15+), so probe instead: a
  // persistent helper thread (started before any parking, see StartMallocProbe) mallocs and frees a
  // spread of sizes on request. If it cannot finish in 25 ms a parked thread holds an allocator lock;
  // a stuck probe completes once the threads are resumed for the retry. No allocation on this side.
  uint64_t gen = ++g_probe_req;
  semaphore_signal(g_probe_sem);
  for (int i = 0; i < 25 && g_probe_done.load() < gen; ++i) usleep(1000);
  if (g_probe_done.load() < gen) return false;
  return true;
}

// REX_SAVEPOINT_PARK_WAITING=1: only accept a park where every guest thread is blocked in the kernel,
// so none is captured part-way through guest code. Diagnostic for Forza (threads parked mid-allocator).
bool AllGuestThreadsWaiting(const std::vector<object_ref<XThread>>& parked) {
  static const bool on = std::getenv("REX_SAVEPOINT_PARK_WAITING") != nullptr;
  if (!on) return true;
  for (auto& t : parked) {
    if (!t->is_guest_thread()) continue;
    arm_thread_state64_t gp{};
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    const mach_port_t port = MachPort(t.get());
    if (thread_get_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&gp), &n) != KERN_SUCCESS ||
        !AtSyscall(port, gp))
      return false;
  }
  return true;
}

std::vector<object_ref<XThread>> ParkThreads(KernelState* ks) {
  StartMallocProbe();
  for (int attempt = 0;; ++attempt) {
    auto parked = ParkThreadsOnce(ks);
    if (attempt < 2000 && ParkedLocksFree() && !AllGuestThreadsWaiting(parked)) {
      for (auto& t : parked) thread_resume(MachPort(t.get()));
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      continue;
    }
    if (ParkedLocksFree() || attempt >= 2000) {
      if (attempt) std::fprintf(stderr, "[savepoint-s3] park: accepted after %d retries (all guest threads waiting: %d)\n", attempt, int(AllGuestThreadsWaiting(parked)));
      return parked;
    }
    for (auto& t : parked) thread_resume(MachPort(t.get()));
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

std::vector<object_ref<XThread>> ParkThreadsOnce(KernelState* ks) {
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

// Host frees are deferred from the capture to the rewind. The rewind revives captured host stack
// frames, and those frames point at host heap blocks (wait-object vectors, lock guards, strings) that
// the code freed after the capture; with frees deferred those blocks still hold their capture-time
// contents and are not reused, so the revived frames free them exactly once. Blocks freed after the
// capture that no revived frame references stay allocated (bounded by one capture-to-rewind window).
// Hooks every malloc zone's free entry points; REX_SAVEPOINT_NO_DEFER_FREE=1 turns this off.
namespace defer_free {
std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_deferred{0}, g_deferred_bytes{0};
struct ZoneHooks {
  malloc_zone_t* zone = nullptr;
  void (*free_)(malloc_zone_t*, void*) = nullptr;
  void (*free_definite_size)(malloc_zone_t*, void*, size_t) = nullptr;
  void* (*realloc_)(malloc_zone_t*, void*, size_t) = nullptr;
  void (*batch_free)(malloc_zone_t*, void**, unsigned) = nullptr;
  void (*try_free_default)(malloc_zone_t*, void*) = nullptr;
};
ZoneHooks g_zones[16];
unsigned g_zone_count = 0;
ZoneHooks* Find(malloc_zone_t* z) {
  for (unsigned i = 0; i < g_zone_count; ++i)
    if (g_zones[i].zone == z) return &g_zones[i];
  return nullptr;
}
// Mode "stack" (default): defer only blocks reachable from the captured host stacks (two levels),
// so the deferral is bounded and can stay on across any number of loads. Mode "all": every free
// between capture and rewind (unbounded; old behaviour). REX_SAVEPOINT_DEFER=all|stack|none.
bool g_all = false;
std::vector<uintptr_t> g_keep;  // sorted; immutable while g_active
inline bool Wanted(void* p) {
  if (g_all) return true;
  return std::binary_search(g_keep.begin(), g_keep.end(), reinterpret_cast<uintptr_t>(p));
}
inline bool Defer(void* p) {
  if (!p || !g_active.load(std::memory_order_relaxed)) return false;
  if (!Wanted(p)) return false;
  g_deferred.fetch_add(1, std::memory_order_relaxed);
  g_deferred_bytes.fetch_add(malloc_size(p), std::memory_order_relaxed);
  return true;
}
void HookFree(malloc_zone_t* z, void* p) {
  if (!Defer(p)) Find(z)->free_(z, p);
}
void HookFreeDefiniteSize(malloc_zone_t* z, void* p, size_t size) {
  if (!Defer(p)) Find(z)->free_definite_size(z, p, size);
}
void HookTryFreeDefault(malloc_zone_t* z, void* p) {
  if (!Defer(p)) Find(z)->try_free_default(z, p);
}
void HookBatchFree(malloc_zone_t* z, void** ptrs, unsigned n) {
  if (!g_active.load(std::memory_order_relaxed)) return Find(z)->batch_free(z, ptrs, n);
  for (unsigned i = 0; i < n; ++i)
    if (!Defer(ptrs[i]) && ptrs[i]) Find(z)->free_(z, ptrs[i]);
}
void* HookRealloc(malloc_zone_t* z, void* p, size_t size) {
  if (!p || !g_active.load(std::memory_order_relaxed) || !Wanted(p)) return Find(z)->realloc_(z, p, size);
  size_t old = malloc_size(p);
  void* n = malloc_zone_malloc(z, size ? size : 1);
  if (n) {
    std::memcpy(n, p, old < size ? old : size);
    Defer(p);
  }
  return n;
}
// Collect heap block starts referenced from [lo, hi) (8-byte aligned words) into out.
void ScanForBlocks(const uint8_t* lo, const uint8_t* hi, std::vector<uintptr_t>& out) {
  for (auto* w = reinterpret_cast<const uintptr_t*>((uintptr_t(lo) + 7) & ~uintptr_t(7));
       reinterpret_cast<const uint8_t*>(w + 1) <= hi; ++w) {
    uintptr_t v = *w;
    if (v < 0x100000000ull || v >= 0x0000800000000000ull || (v & 15)) continue;
    if (malloc_size(reinterpret_cast<void*>(v)) == 0) continue;
    out.push_back(v);
  }
}
void BuildKeepSet(const std::vector<std::pair<const uint8_t*, const uint8_t*>>& stacks) {
  std::vector<uintptr_t> level;
  for (auto& [lo, hi] : stacks) ScanForBlocks(lo, hi, level);
  std::sort(level.begin(), level.end());
  level.erase(std::unique(level.begin(), level.end()), level.end());
  std::vector<uintptr_t> all = level;
  for (int depth = 1; depth < 3 && !level.empty(); ++depth) {
    std::vector<uintptr_t> next;
    for (uintptr_t b : level) {
      size_t n = malloc_size(reinterpret_cast<void*>(b));
      if (n > (1u << 20)) n = 1u << 20;
      ScanForBlocks(reinterpret_cast<const uint8_t*>(b), reinterpret_cast<const uint8_t*>(b) + n, next);
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    std::vector<uintptr_t> fresh;
    std::set_difference(next.begin(), next.end(), all.begin(), all.end(), std::back_inserter(fresh));
    std::vector<uintptr_t> merged;
    std::merge(all.begin(), all.end(), fresh.begin(), fresh.end(), std::back_inserter(merged));
    all.swap(merged);
    level.swap(fresh);
  }
  g_keep.swap(all);
}
void Install() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  vm_address_t* zones = nullptr;
  unsigned count = 0;
  if (malloc_get_all_zones(mach_task_self(), nullptr, &zones, &count) != KERN_SUCCESS) return;
  const uintptr_t page = uintptr_t(getpagesize());
  for (unsigned i = 0; i < count && g_zone_count < 16; ++i) {
    auto* z = reinterpret_cast<malloc_zone_t*>(zones[i]);
    uintptr_t lo = uintptr_t(z) & ~(page - 1), hi = (uintptr_t(z + 1) + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE) != 0) continue;
    ZoneHooks& h = g_zones[g_zone_count++];
    h.zone = z;
    h.free_ = z->free;
    h.realloc_ = z->realloc;
    h.free_definite_size = z->free_definite_size;
    h.batch_free = z->batch_free;
    h.try_free_default = z->version >= 13 ? z->try_free_default : nullptr;
    z->free = HookFree;
    z->realloc = HookRealloc;
    if (h.free_definite_size) z->free_definite_size = HookFreeDefiniteSize;
    if (h.batch_free) z->batch_free = HookBatchFree;
    if (h.try_free_default) z->try_free_default = HookTryFreeDefault;
  }
  std::fprintf(stderr, "[savepoint-s3] free deferral hooked %u malloc zones\n", g_zone_count);
}
}  // namespace defer_free

std::vector<XThread*> g_held;
Clock::time_point g_capture_time;
uint64_t g_capture_guest_ticks = 0;

namespace cs_trace { void SetPhase(uint32_t p); }

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
  g_snap.dispatch_queue = ks->CaptureDispatchQueue();
  g_snap.dpc_head = ks->dpc_list()->head();  // head is host-side; the entries are guest memory
  if (auto* runtime = rex::Runtime::instance()) {
    g_snap.graphics = runtime->graphics_system();
    if (g_snap.graphics) g_snap.graphics->CaptureSavepoint();
    // XMA contexts are allocated and kicked host-side; without this a voice the game created or freed
    // after the capture leaves two voices sharing one context after the rewind (REX_SAVEPOINT_NO_XMA=1 off).
    auto* audio = dynamic_cast<audio::AudioSystem*>(runtime->audio_system());
    if (audio && audio->xma_decoder() && !std::getenv("REX_SAVEPOINT_NO_XMA")) {
      g_snap.xma_decoder = audio->xma_decoder();
      g_snap.xma = std::make_shared<audio::XmaDecoder::RewindState>();
      g_snap.xma_decoder->CaptureRewindState(g_snap.xma.get());
    }
  }
  const uint32_t bases[] = {0x00000000, 0x40000000, 0x80000000, 0x90000000};
  for (uint32_t b : bases) {
    if (auto* heap = mem->LookupHeap(b)) {
      g_snap.heaps.push_back({heap, heap->CaptureRewindState()});
      CollectRegions(heap, 0, mem->virtual_membase(), g_snap.regions);
    }
  }
  // The 0xA0000000/0xC0000000/0xE0000000 views keep their own page tables on top of the physical
  // heap. Without rewinding them, a physical allocation made or freed after the capture leaves the
  // view and the physical heap disagreeing, and later physical allocations fail (guest gets 0).
  if (!std::getenv("REX_SAVEPOINT_SKIP_VIEW_HEAPS")) {
    for (uint32_t b : {0xA0000000u, 0xC0000000u, 0xE0000000u}) {
      if (auto* heap = mem->LookupHeap(b)) g_snap.heaps.push_back({heap, heap->CaptureRewindState()});
    }
  }
  g_snap.fiber_map = ks->CaptureFibers();
  rex::thread::Fiber::hold_destroyed = true;
  for (auto& [addr, info] : g_snap.fiber_map) {
    auto* f = info.host_fiber;
    if (!f) continue;
    InProcess::FiberSnap fs{f};
    fs.object.assign(reinterpret_cast<uint8_t*>(f), reinterpret_cast<uint8_t*>(f) + sizeof(*f));
    // Thread fibers run on the pthread stack, which the thread capture below covers.
    if (!info.is_thread_fiber) fs.stack.assign(f->stack_data(), f->stack_data() + f->stack_size());
    g_snap.fibers.push_back(std::move(fs));
  }
  std::fprintf(stderr, "[savepoint-fiber] captured %zu fibers\n", g_snap.fibers.size());
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
    if (std::getenv("REX_SAVEPOINT_DIAG")) {
      // Walk the captured frame-pointer chain: every host frame listed here is revived by the rewind,
      // together with any host heap object it points to.
      std::fprintf(stderr, "[savepoint-diag] thread %zu frames:", g_snap.threads.size());
      uint64_t pcs[24]; int n = 0;
      pcs[n++] = arm_thread_state64_get_pc(ts.gp);
      pcs[n++] = arm_thread_state64_get_lr(ts.gp);
      uint64_t fp = arm_thread_state64_get_fp(ts.gp);
      while (n < 24 && fp >= ts.stack_lo && fp + 16 <= ts.stack_hi) {
        uint64_t next = *reinterpret_cast<uint64_t*>(fp), ret = *reinterpret_cast<uint64_t*>(fp + 8);
        pcs[n++] = ret;
        if (next <= fp) break;
        fp = next;
      }
      for (int i = 0; i < n; ++i) {
        Dl_info di{};
        const char* name = (dladdr(reinterpret_cast<void*>(pcs[i] & 0x0000FFFFFFFFFFFFull), &di) && di.dli_sname) ? di.dli_sname : "?";
        std::fprintf(stderr, " | %.90s", name);
      }
      std::fprintf(stderr, "\n");
    }
    g_snap.threads.push_back(std::move(ts));
  }
  g_snap.valid = true;
  g_capture_guest_ticks = rex::chrono::Clock::QueryGuestTickCount();
  cs_trace::SetPhase(1);
  {
    const char* mode = std::getenv("REX_SAVEPOINT_DEFER");
    if (std::getenv("REX_SAVEPOINT_NO_DEFER_FREE")) mode = "none";
    if (!mode) mode = "stack";
    defer_free::g_active.store(false);
    if (std::strcmp(mode, "none") != 0) {
      defer_free::Install();
      defer_free::g_deferred = 0;
      defer_free::g_deferred_bytes = 0;
      defer_free::g_all = std::strcmp(mode, "all") == 0;
      if (!defer_free::g_all) {
        std::vector<std::pair<const uint8_t*, const uint8_t*>> stacks;
        for (auto& ts : g_snap.threads)
          stacks.push_back({ts.stack.data(), ts.stack.data() + ts.stack.size()});
        defer_free::BuildKeepSet(stacks);
        std::fprintf(stderr, "[savepoint-s3] free deferral (stack mode): %zu blocks reachable from captured stacks\n",
                     defer_free::g_keep.size());
      }
      defer_free::g_active.store(true);
    }
  }
  auto t2 = Clock::now();
  // Bisect switch REX_SAVEPOINT_HOLD=guest|host: keep guest threads (or host XHostThreads) suspended
  // from capture to rewind, so only the other side can change state in between.
  const char* hold = std::getenv("REX_SAVEPOINT_HOLD");
  g_held.clear();
  for (auto& t : parked) {
    bool held = hold && ((std::strcmp(hold, "guest") == 0 && t->is_guest_thread()) ||
                         (std::strcmp(hold, "host") == 0 && !t->is_guest_thread()));
    if (held) {
      g_held.push_back(t.get());
      continue;
    }
    thread_resume(MachPort(t.get()));
  }
  if (hold) std::fprintf(stderr, "[savepoint-s3] holding %zu %s threads until the rewind\n", g_held.size(), hold);
  g_capture_time = Clock::now();
  st.threads_parked = uint32_t(g_snap.threads.size());
  st.park_ms = Ms(t0, t1);
  st.write_ms = Ms(t1, t2);
  st.ok = true;
  return st;
}

// If the rewind stalls, print the rewinding thread's host frames (no malloc on this path).
std::atomic<int> g_rewind_phase{0};
void StartRewindWatchdog(std::atomic<bool>* done) {
  mach_port_t target = mach_thread_self();
  pthread_t watcher;
  struct Args { mach_port_t target; std::atomic<bool>* done; };
  static Args args;
  args = {target, done};
  pthread_create(&watcher, nullptr, [](void* a) -> void* {
    auto* args = static_cast<Args*>(a);
    for (int i = 0; i < 150 && !args->done->load(); ++i) usleep(100000);
    if (args->done->load()) return nullptr;
    thread_suspend(args->target);
    arm_thread_state64_t gp{};
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    thread_get_state(args->target, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&gp), &n);
    char line[256];
    int len = snprintf(line, sizeof line, "[savepoint-s3] REWIND STUCK in phase %d, frames:\n", g_rewind_phase.load());
    write(2, line, len);
    uint64_t pcs[32]; int k = 0;
    pcs[k++] = arm_thread_state64_get_pc(gp);
    pcs[k++] = arm_thread_state64_get_lr(gp);
    uint64_t fp = arm_thread_state64_get_fp(gp);
    while (k < 32 && fp && (fp & 7) == 0) {
      uint64_t next = reinterpret_cast<uint64_t*>(fp)[0], ret = reinterpret_cast<uint64_t*>(fp)[1];
      pcs[k++] = ret;
      if (next <= fp) break;
      fp = next;
    }
    for (int i = 0; i < k; ++i) {
      Dl_info di{};
      const char* name = (dladdr(reinterpret_cast<void*>(pcs[i] & 0x0000FFFFFFFFFFFFull), &di) && di.dli_sname) ? di.dli_sname : "?";
      len = snprintf(line, sizeof line, "  %d %.200s\n", i, name);
      write(2, line, len);
    }
    thread_resume(args->target);
    return nullptr;
  }, &args);
  pthread_detach(watcher);
}

// Diagnostic, called from the unhandled guest access violation handler: what changed in guest memory
// since the last rewind restored it. Prints changed words on the faulting allocator's pages, and a count
// of changed pages overall. Runs once.

namespace cs_trace {
struct Entry {
  uint32_t seq, thread, lr, phase;
  uint32_t regs[7];  // r3 r4 r5 r28 r29 r30 r31
  uint32_t chain[8];
};
constexpr uint32_t kSize = 1u << 19;
std::atomic<uint32_t> g_next{0};
std::atomic<uint32_t> g_phase{0};  // 0 off, 1 after capture, 2 after rewind
Entry* g_ring = nullptr;
uint32_t Target() {
  static const uint32_t t = [] {
    const char* e = std::getenv("REX_SAVEPOINT_TRACE_CS");
    return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
  }();
  return t;
}
void SetPhase(uint32_t p) {
  if (!Target()) return;
  if (!g_ring) g_ring = new Entry[kSize]();
  if (p == 1) g_next = 0;
  g_phase = p;
}
void Dump() {
  const char* path = std::getenv("REX_SAVEPOINT_TRACE_OUT");
  if (!g_ring || !path) return;
  FILE* f = std::fopen(path, "w");
  if (!f) return;
  const uint32_t n = std::min(g_next.load(), kSize);
  for (uint32_t i = 0; i < n; ++i) {
    const Entry& e = g_ring[i];
    std::fprintf(f, "%u p%u t%08X lr %08X r3 %08X r4 %08X r5 %08X r28 %08X r29 %08X r30 %08X r31 %08X |", e.seq,
                 e.phase, e.thread, e.lr, e.regs[0], e.regs[1], e.regs[2], e.regs[3], e.regs[4], e.regs[5], e.regs[6]);
    for (uint32_t c : e.chain) {
      if (!c) break;
      std::fprintf(f, " %08X", c);
    }
    std::fputc('\n', f);
  }
  std::fclose(f);
  std::fprintf(stderr, "[savepoint-cstrace] dumped %u entries to %s\n", n, path);
}
}  // namespace cs_trace

void SavepointTraceCs(uint32_t cs) {
  const uint32_t phase = cs_trace::g_phase.load(std::memory_order_relaxed);
  if (!phase || cs != cs_trace::Target()) return;
  auto* ts = runtime::ThreadState::Get();
  if (!ts) return;
  const uint32_t i = cs_trace::g_next.fetch_add(1, std::memory_order_relaxed);
  if (i >= cs_trace::kSize) return;
  auto& e = cs_trace::g_ring[i];
  const auto* ctx = ts->context();
  e = {};
  e.seq = i;
  e.phase = phase;
  e.thread = XThread::IsInThread() ? XThread::GetCurrentThread()->guest_object() : 0;
  e.lr = uint32_t(ctx->lr);
  const uint32_t r[7] = {ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r28.u32, ctx->r29.u32, ctx->r30.u32, ctx->r31.u32};
  std::memcpy(e.regs, r, sizeof(r));
  auto* membase = kernel_state()->memory()->virtual_membase();
  uint32_t sp = ctx->r1.u32;
  for (int k = 0; k < 8; ++k) {
    if (sp < 0x40000000u || sp >= 0x7F000000u) break;
    uint32_t back;
    std::memcpy(&back, membase + sp, 4);
    back = rex::byte_swap(back);
    if (back <= sp || back >= 0x7F000000u) break;
    uint32_t lr;
    std::memcpy(&lr, membase + back - 8, 4);
    e.chain[k] = rex::byte_swap(lr);
    sp = back;
  }
}

void SavepointCrashDiff(uint32_t fault, uint32_t r3, uint32_t r4) {
  static std::atomic<bool> done{false};
  if (!g_snap.valid || done.exchange(true)) return;
  cs_trace::g_phase = 0;
  cs_trace::Dump();
  auto* mem = kernel_state()->memory();
  uint64_t changed_pages = 0;
  for (auto& r : g_snap.regions) {
    const uint8_t* base = r.space ? mem->physical_membase() : mem->virtual_membase();
    for (uint32_t off = 0; off < r.size; off += 4096)
      if (std::memcmp(base + r.base + off, r.data.data() + off,
                      std::min<uint32_t>(4096, r.size - off)) != 0) ++changed_pages;
  }
  std::fprintf(stderr, "[savepoint-crashdiff] fault %08X r3 %08X r4 %08X: %llu guest pages changed since capture\n",
               fault, r3, r4, (unsigned long long)changed_pages);
  const uint32_t pages[] = {r3 & ~0xFFFu, (r3 & ~0xFFFu) + 0x1000, r4 & ~0xFFFu, (r4 - 0x20) & ~0xFFFu};
  for (size_t i = 0; i < 4; ++i) {
    if (i == 3 && pages[3] == pages[2]) break;
    const uint32_t page = pages[i];
    for (auto& r : g_snap.regions) {
      if (r.space || page < r.base || page >= r.base + r.size) continue;
      const uint32_t off = page - r.base;
      const uint32_t len = std::min<uint32_t>(4096, r.size - off);
      int shown = 0;
      for (uint32_t w = 0; w + 4 <= len; w += 4) {
        uint32_t now, then;
        std::memcpy(&now, mem->virtual_membase() + page + w, 4);
        std::memcpy(&then, r.data.data() + off + w, 4);
        if (now != then && shown++ < 64)
          std::fprintf(stderr, "[savepoint-crashdiff]   %08X: %08X -> %08X\n", page + w,
                       rex::byte_swap(then), rex::byte_swap(now));
      }
      std::fprintf(stderr, "[savepoint-crashdiff] page %08X: %d words changed\n", page, shown);
    }
  }
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
  static std::atomic<bool> rewind_done{false};
  g_rewind_phase = 1;
  StartRewindWatchdog(&rewind_done);
  auto parked = ParkThreads(ks);
  for (auto* t : g_held) thread_resume(MachPort(t));  // ParkThreads suspended them a second time
  cs_trace::SetPhase(2);  // threads parked: later entries belong to the rewound timeline
  if (!std::getenv("REX_SAVEPOINT_NO_CLOCK")) {
    // Guest time is guest state: without this the rewound frame sees the whole save-to-rewind gap as elapsed.
    rex::chrono::Clock::RewindGuestTickCount(g_capture_guest_ticks);
  }
  if (!g_held.empty()) std::fprintf(stderr, "[savepoint-s3] released hold on %zu threads\n", g_held.size());
  g_held.clear();
  g_rewind_phase = 2;
  std::fprintf(stderr, "[savepoint-s3] rewind start, %zu parked\n", parked.size());
  auto t1 = Clock::now();
  const bool restore_objects = std::getenv("REX_SAVEPOINT_SKIP_OBJECTS") == nullptr;
  if (restore_objects) {
    g_snap.objects_at_rewind = ks->object_table()->GetAllObjects();
    g_rewind_phase = 3;
    ks->object_table()->Rewind(g_snap.object_table);
    size_t queued = ks->RewindDispatchQueue(g_snap.dispatch_queue);
    std::fprintf(stderr, "[savepoint-s3] kernel dispatch queue: %zu -> %zu\n", queued, g_snap.dispatch_queue.size());
    std::fprintf(stderr, "[savepoint-s3] dpc list head: %08X -> %08X\n", ks->dpc_list()->head(), g_snap.dpc_head);
    ks->dpc_list()->set_head(g_snap.dpc_head);
    g_rewind_phase = 4;
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
    if (std::getenv("REX_SAVEPOINT_DIAG")) {
      uint32_t changed = 0, first = 0;
      for (uint32_t off = 0; off < r.size; off += 4096) {
        uint32_t len = std::min<uint32_t>(4096, r.size - off);
        if (std::memcmp(base + r.base + off, r.data.data() + off, len) != 0) {
          if (!changed) first = r.base + off;
          ++changed;
        }
      }
      if (changed)
        std::fprintf(stderr, "[savepoint-diag] region %u:%08X +%X prot %u: %u pages changed, first %08X\n",
                     r.space, r.base, r.size, r.protect, changed, first);
    }
    std::memcpy(base + r.base, r.data.data(), r.size);
    for (auto& t : touched) heap->Protect(t.first, t.second, r.protect);
    st.bytes += r.size;
  }
  if (restore_memory) {
    const bool reconcile = std::getenv("REX_SAVEPOINT_NO_ZERO") == nullptr;
    for (const auto& [heap, heap_state] : g_snap.heaps) {
      // Views (A0/C0/E0) alias the physical heap's memory, which the parent's pass already zeroes.
      heap->Rewind(heap_state, reconcile && heap->heap_base() < 0xA0000000u);
    }
    if (!std::getenv("REX_SAVEPOINT_NO_FIBERS")) {
      ks->RewindFibers(g_snap.fiber_map);
      for (auto& fs : g_snap.fibers) {
        std::memcpy(reinterpret_cast<void*>(fs.fiber), fs.object.data(), fs.object.size());
        if (!fs.stack.empty()) std::memcpy(fs.fiber->stack_data(), fs.stack.data(), fs.stack.size());
      }
      std::fprintf(stderr, "[savepoint-fiber] rewound %zu fibers\n", g_snap.fibers.size());
    }
    g_rewind_phase = 5;
    if (g_snap.graphics) g_snap.graphics->RewindSavepoint();
    if (g_snap.xma) {
      std::fprintf(stderr, "[savepoint-xma] rewound XMA: %u contexts changed allocation/enable since capture\n",
                   g_snap.xma_decoder->Rewind(*g_snap.xma));
    }
    g_rewind_phase = 6;
    // Check that every captured byte really is back after the heap and graphics rewinds ran.
    uint64_t bad_pages = 0; uint32_t bad_regions = 0;
    for (auto& r : g_snap.regions) {
      uint8_t* base = r.space ? mem->physical_membase() : mem->virtual_membase();
      uint32_t bad = 0, first = 0;
      for (uint32_t off = 0; off < r.size; off += 4096) {
        uint32_t len = std::min<uint32_t>(4096, r.size - off);
        if (std::memcmp(base + r.base + off, r.data.data() + off, len) != 0) { if (!bad) first = r.base + off; ++bad; }
      }
      if (bad) {
        ++bad_regions; bad_pages += bad;
        if (bad_regions <= 20) std::fprintf(stderr, "[savepoint-verify] region %u:%08X +%X: %u pages differ after restore, first %08X\n", r.space, r.base, r.size, bad, first);
      }
    }
    std::fprintf(stderr, "[savepoint-verify] memory after restore: %u regions / %llu pages differ\n", bad_regions, (unsigned long long)bad_pages);
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
    std::fprintf(stderr, "[savepoint-s3] thread %u id %08X guest %d port %u syscall %d x16 %lld detach %s %.2f ms\n",
                 restored + not_parked, ts.thread->thread_id(), int(ts.thread->is_guest_thread()), ts.port, int(ts.in_syscall), (long long)ts.gp.__x[16],
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
  // Mode "all": revived frames free their blocks for real from here; the deferred ones stay leaked.
  // Mode "stack": stays on, so a second load of the same savepoint still finds those blocks intact.
  if (defer_free::g_all) defer_free::g_active.store(false);
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
  rewind_done = true;
  std::fprintf(stderr, "[savepoint-s3] rewind done, %u threads left parked\n", left_parked);
  std::fprintf(stderr, "[savepoint-s3] frees deferred since capture: %llu (%llu KB)\n",
               (unsigned long long)defer_free::g_deferred.load(),
               (unsigned long long)(defer_free::g_deferred_bytes.load() >> 10));
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

namespace control {
std::mutex g_m;
std::condition_variable g_cv;
int g_op = 0;  // 1 save, 2 load
uint64_t g_seq = 0, g_done = 0;
std::string g_result;
int g_loads = 0;
}  // namespace control

std::string ControlInProcessSavepoint(const std::string& op, int timeout_ms) {
  int code = op == "save" ? 1 : op == "load" ? 2 : 0;
  if (!code) return "error usage: savepoint save|load";
  std::unique_lock<std::mutex> l(control::g_m);
  if (control::g_op) return "error busy";
  control::g_op = code;
  uint64_t seq = ++control::g_seq;
  if (!control::g_cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return control::g_done >= seq; })) {
    control::g_op = 0;
    return "error timeout (no command-processor safe point; is the game rendering?)";
  }
  return control::g_result;
}

void ServiceControlRequest(int frame) {
  int op;
  uint64_t seq;
  {
    std::lock_guard<std::mutex> l(control::g_m);
    op = control::g_op;
    seq = control::g_seq;
  }
  if (!op) return;
  char buf[200];
  if (op == 1) {
    auto st = CaptureInProcessSavepoint();
    control::g_loads = 0;
    std::snprintf(buf, sizeof buf, "ok save frame=%d mb=%llu ms=%.1f threads=%u", frame,
                  (unsigned long long)(st.bytes >> 20), st.park_ms + st.write_ms, st.threads_parked);
  } else if (!g_snap.valid) {
    std::snprintf(buf, sizeof buf, "error no savepoint (send savepoint save first)");
  } else {
    auto st = RewindToInProcessSavepoint();
    ++control::g_loads;
    std::snprintf(buf, sizeof buf, "ok load frame=%d ms=%.1f threads=%u loads=%d", frame,
                  st.park_ms + st.write_ms, st.threads_parked, control::g_loads);
  }
  REXSYS_INFO("[savepoint-s3] control: {}", buf);
  std::lock_guard<std::mutex> l(control::g_m);
  control::g_result = buf;
  control::g_done = seq;
  control::g_op = 0;
  control::g_cv.notify_all();
}

void ServiceSavepointRequest() {
  int frame = g_frame.load(std::memory_order_relaxed);
  ServiceControlRequest(frame);
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
      // Diagnostic: rewind straight after the capture, so almost nothing has run since. A failure here
      // is in the restore mechanism itself, not in state that drifted between capture and rewind.
      if (std::getenv("REX_SAVEPOINT_REWIND_AT_CAPTURE")) {
        rewound = true;
        auto rst = RewindToInProcessSavepoint();
        std::fprintf(stderr, "[savepoint-s3] rewound at capture frame %d: ok=%d\n", frame, int(rst.ok));
      }
    } else if (captured && !rewound &&
               ((rew >= 0 && frame >= rew) ||
                (std::getenv("REX_SAVEPOINT_REWIND_AFTER_MS") &&
                 Ms(g_capture_time, Clock::now()) >= std::atof(std::getenv("REX_SAVEPOINT_REWIND_AFTER_MS"))))) {
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
