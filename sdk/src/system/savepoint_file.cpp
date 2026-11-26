/**
 * @file        system/savepoint_file.cpp
 * @brief       Fresh-process save points: the in-process capture written to a file, loaded at launch
 *
 * File (little endian): "REXSP2\0\0", u32 version, u8 uuid[16] (main executable LC_UUID ^ librexruntime LC_UUID), u32 frame,
 *   u32 heap count, per heap {u32 base, u32 physical, u32 unreserved pages, u32 n, u64 page entries[n]},
 *   u32 region count, per region {u32 space (0 virtual, 1 physical), u32 base, u32 size, u32 protect,
 *   bytes}, u32 GPU state size, GPU state bytes (GraphicsFileHook), u32 'END!'.
 * Physical bytes already carry the pages only the GPU copy held (resolves are not read back to guest
 * memory). Function tables (host pointers) are written but never loaded.
 */

#include <rex/system/savepoint.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <algorithm>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <libunwind.h>
#include <mach/mach.h>
#include <pthread.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/audio/audio_system.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/chrono/clock.h>
#include <rex/stream.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xevent.h>
#include <rex/system/user_module.h>
#include <rex/system/xmodule.h>
#include <rex/system/xobject.h>

REXCVAR_DEFINE_STRING(load_savepoint, "", "Test",
                      "Start from this save point file (written by 'savepoint file PATH') instead of booting");

extern "C" uintptr_t __stack_chk_guard;  // libsystem_c, shared by every image in the process

namespace rex::kernel::crt {
rex::thread::Fiber* RecreateGuestFiber(rex::system::KernelState* ks, uint32_t start_address, uint32_t guest_fiber_addr,
                                      size_t host_stack);
}

namespace rex::system {

// savepoint.cpp
extern std::function<void()> g_capture_parked_hook;
void VisitInProcessThreads(const std::function<void(XThread*, const arm_thread_state64_t&,
                                                    const arm_neon_state64_t&, void* ctx)>& fn);
void VisitInProcessSnapshot(
    const std::function<void(memory::BaseHeap*, const memory::BaseHeap::RewindState&)>& heap,
    const std::function<void(uint32_t space, uint32_t base, uint32_t size, uint32_t protect,
                             const uint8_t* data)>& region);

// Load errors also go to stderr: a bad save point often dies before the log is flushed.
#define SPLOAD_ERR(...)                                  \
  do {                                                   \
    const std::string sp_msg_ = fmt::format(__VA_ARGS__); \
    REXSYS_ERROR("{}", sp_msg_);                         \
    std::fprintf(stderr, "%s\n", sp_msg_.c_str());       \
  } while (0)

namespace {
using Clock = std::chrono::steady_clock;
constexpr char kMagic[8] = {'R', 'E', 'X', 'S', 'P', '2', 0, 0};
constexpr uint32_t kVersion = 11;  // 10: fibers, 11: procedure thunks
constexpr uint64_t kArenaSize = 0x120000000ull;  // virtual + physical guest views (xmemory.cpp)
constexpr uint32_t kEnd = 0x21444E45;  // "END!"

std::atomic<IGraphicsSystem*> g_gfx{nullptr};
std::atomic<GraphicsFileHook> g_gfx_hook{nullptr};

double Ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

bool ImageUuid(const mach_header_64* h, uint8_t out[16]) {
  if (!h) return false;
  auto* lc = reinterpret_cast<const load_command*>(h + 1);
  for (uint32_t i = 0; i < h->ncmds; ++i) {
    if (lc->cmd == LC_UUID) {
      std::memcpy(out, reinterpret_cast<const uuid_command*>(lc)->uuid, 16);
      return true;
    }
    lc = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
  }
  return false;
}

// Build fingerprint: the executable's LC_UUID mixed with librexruntime's, since the runtime is a separate dylib
// that can be rebuilt (struct layouts in the saved objects change) while the executable stays the same.
bool MainUuid(uint8_t out[16]) {
  if (!ImageUuid(reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0)), out)) return false;
  for (uint32_t i = 1; i < _dyld_image_count(); ++i) {
    const char* name = _dyld_get_image_name(i);
    uint8_t lib[16];
    if (name && std::strstr(name, "librexruntime") &&
        ImageUuid(reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i)), lib)) {
      for (int k = 0; k < 16; ++k) out[k] ^= lib[k];
      break;
    }
  }
  return true;
}

struct Out {
  int fd;
  bool ok = true;
  uint64_t bytes = 0;
  void Put(const void* p, size_t n) {
    auto* c = static_cast<const uint8_t*>(p);
    while (ok && n) {
      ssize_t w = ::write(fd, c, n);
      if (w <= 0) {
        ok = false;
        break;
      }
      c += w, n -= size_t(w), bytes += uint64_t(w);
    }
  }
  void U32(uint32_t v) { Put(&v, 4); }
  void Str(const std::string& v) { U32(uint32_t(v.size())), Put(v.data(), v.size()); }
};

struct In {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  const uint8_t* Take(size_t n) {
    if (!ok || size_t(end - p) < n) {
      ok = false;
      return nullptr;
    }
    auto* r = p;
    p += n;
    return r;
  }
  uint32_t U32() {
    uint32_t v = 0;
    if (auto* r = Take(4)) std::memcpy(&v, r, 4);
    return v;
  }
  std::string Str() {
    uint32_t n = U32();
    auto* r = Take(n);
    return r ? std::string(reinterpret_cast<const char*>(r), n) : std::string();
  }
};

memory::BaseHeap* HeapFor(memory::Memory* mem, uint32_t base, bool physical) {
  return physical ? mem->GetPhysicalHeap() : mem->LookupHeap(base);
}

// Puts each committed run's host access back in line with the (restored) page table.
// Stage-2 survey: every kernel object, and whether its legacy Save handler takes it.
void LogObjects(const char* when) {
  std::vector<uint8_t> buf(16u << 20);
  for (auto& o : Runtime::instance()->kernel_state()->object_table()->GetAllObjects()) {
    stream::ByteStream bs(buf.data(), buf.size());
    bool saved = !o->is_host_object() && o->Save(&bs);
    REXSYS_INFO("[savepoint-objs] {} handle={:08X} type={} host={} guest_obj={:08X} saved={} bytes={} name='{}'", when,
                o->handle(), int(o->type()), o->is_host_object(), o->guest_object(), saved, bs.offset(), o->name());
  }
}

void SyncHeapAccess(memory::BaseHeap* heap) {
  uint64_t a = heap->heap_base(), end = uint64_t(heap->heap_base()) + heap->heap_size();
  while (a < end) {
    memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(a), &info)) break;
    uint32_t size = info.region_size ? info.region_size : heap->page_size();
    if (info.state & memory::kMemoryAllocationCommit) heap->Protect(uint32_t(a), size, info.protect);
    a += size;
  }
}
// --- Threads -------------------------------------------------------------------------------------------
// A guest thread's host stack holds, oldest first: SDK start frames (XThread::Execute and below), the
// recompiled-code frames (the band), and for a blocked thread an import call (HostToGuestFunction ...).
// The band is copied into the new thread's stack with its pointers moved (stack, guest arena, ctx, game
// image); a blocked thread restarts at the `bl` into its import, so the wait is issued again. The oldest
// band frame returns into rex_sp_landing, which ends the thread as XThread::Execute would.
struct ThreadRec {
  uint32_t handle = 0, blocked = 0;
  arm_thread_state64_t gp{};
  arm_neon_state64_t neon{};
  uint64_t band_lo = 0, band_top = 0, record = 0, ctx = 0;
  uint64_t tls_lo = 0, tls_hi = 0;  // the executable's thread-local block on the saving thread
  std::vector<uint8_t> ctx_bytes, band;
};

// A guest fiber (rexcrt hooks): thread fibers name their thread; others are fresh (never run) or suspended in
// SwitchToFiber with their host stack band in rec.
struct FiberRec {
  uint32_t addr = 0, uid = 0, stack_base = 0, stack_bottom = 0, start = 0, owner = 0, kind = 0;  // kind 0/1/2
  uint64_t host_stack = 0;
  ThreadRec rec;
};
enum : uint32_t { kThreadFiber = 0, kFreshFiber = 1, kSuspendedFiber = 2 };

// The executable's thread-local block for the calling thread, {lo, hi} (0, 0 when it has none). Native game
// code keeps pointers into it in callee-saved registers, so moved stacks need it mapped old -> new.
std::pair<uint64_t, uint64_t> ExeTlsBlock() {
  struct Tlv {
    void* (*thunk)(Tlv*);
    unsigned long key, offset;
  };
  auto* h = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
  const uint64_t slide = uint64_t(_dyld_get_image_vmaddr_slide(0));
  Tlv* first = nullptr;
  uint64_t size = 0;
  auto* lc = reinterpret_cast<const load_command*>(h + 1);
  for (uint32_t i = 0; i < h->ncmds; ++i) {
    if (lc->cmd == LC_SEGMENT_64) {
      auto* seg = reinterpret_cast<const segment_command_64*>(lc);
      auto* sect = reinterpret_cast<const section_64*>(seg + 1);
      for (uint32_t k = 0; k < seg->nsects; ++k) {
        uint32_t type = sect[k].flags & SECTION_TYPE;
        if (type == S_THREAD_LOCAL_VARIABLES && sect[k].size >= sizeof(Tlv) && !first)
          first = reinterpret_cast<Tlv*>(sect[k].addr + slide);
        if (type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL) size += sect[k].size;
      }
    }
    lc = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
  }
  if (!first || !size) return {0, 0};
  // dyld4 keeps other bits above the offset (seen: 0x1170'0000'07C8); the offset itself is the low 32.
  const uint64_t offset = first->offset & 0xFFFFFFFFull;
  if (offset >= size) return {0, 0};
  uint64_t lo = reinterpret_cast<uint64_t>(first->thunk(first)) - offset;
  return {lo, lo + size};
}

std::mutex g_tls_m;
std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> g_tls;  // guest thread handle -> its block

std::pair<uint64_t, uint64_t> ExeRange() {
  auto* h = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
  uint64_t lo = ~0ull, hi = 0, slide = uint64_t(_dyld_get_image_vmaddr_slide(0));
  auto* lc = reinterpret_cast<const load_command*>(h + 1);
  for (uint32_t i = 0; i < h->ncmds; ++i) {
    if (lc->cmd == LC_SEGMENT_64) {
      auto* seg = reinterpret_cast<const segment_command_64*>(lc);
      if (seg->vmaddr || seg->vmsize < (1ull << 32)) {  // not __PAGEZERO
        lo = std::min(lo, seg->vmaddr), hi = std::max(hi, seg->vmaddr + seg->vmsize);
      }
    }
    lc = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
  }
  return {lo + slide, hi + slide};
}

// The executable's writable segments (globals, bss), with this process's slide.
std::vector<std::pair<uint64_t, uint64_t>> ExeDataRanges() {
  std::vector<std::pair<uint64_t, uint64_t>> r;
  auto* h = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
  const uint64_t slide = uint64_t(_dyld_get_image_vmaddr_slide(0));
  auto* lc = reinterpret_cast<const load_command*>(h + 1);
  for (uint32_t i = 0; i < h->ncmds; ++i) {
    auto* seg = reinterpret_cast<const segment_command_64*>(lc);
    if (lc->cmd == LC_SEGMENT_64 && (seg->initprot & VM_PROT_WRITE) && seg->vmsize)
      r.push_back({seg->vmaddr + slide, seg->vmaddr + seg->vmsize + slide});
    lc = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
  }
  return r;
}

// A host function as {image file name, offset}: images load at another address in the next process.
bool FuncToFile(::PPCFunc* f, std::string& image, uint64_t& offset) {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(f), &info) || !info.dli_fname) return false;
  image = std::filesystem::path(info.dli_fname).filename().string();
  offset = reinterpret_cast<uint64_t>(f) - reinterpret_cast<uint64_t>(info.dli_fbase);
  return true;
}
::PPCFunc* FuncFromFile(const std::string& image, uint64_t offset) {
  for (uint32_t i = 0; i < _dyld_image_count(); ++i)
    if (std::filesystem::path(_dyld_get_image_name(i)).filename() == image)
      return reinterpret_cast<::PPCFunc*>(reinterpret_cast<uint64_t>(_dyld_get_image_header(i)) + offset);
  return nullptr;
}

std::string Sym(uint64_t pc) {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(pc), &info)) return "?";
  return info.dli_sname ? info.dli_sname : info.dli_fname ? info.dli_fname : "?";
}
bool InSystemLib(uint64_t pc) {
  Dl_info info{};
  return dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname &&
         std::strncmp(info.dli_fname, "/usr/lib/system/", 16) == 0;
}

// A wait parked between its wake-up and its return may already have taken the object's signal (an auto-reset
// event resets as the waiter leaves): re-issuing it in the new process would lose that wake. Only a thread still
// in the kernel wait, or in a yield/sleep where nothing is taken, is safe to re-issue.
bool StillWaiting(XThread* t, const arm_thread_state64_t& gp) {
  const uint64_t pc = arm_thread_state64_get_pc(gp);
  uint32_t insn = 0;
  if (pc < 4) return false;
  std::memcpy(&insn, reinterpret_cast<const void*>(pc - 4), 4);
  if (insn != 0xD4001001u) return false;  // svc #0x80: parked at a syscall
  const std::string leaf = Sym(pc);
  if (leaf == "__semwait_signal" || leaf == "mach_wait_until" || leaf == "swtch_pri") return true;
  // Condition waits, and socket waits of networked games (kevent64/select/poll): still blocked if the thread is.
  // e.g. __psynch_mutexwait after a cond wake is in transit.
  if (leaf != "__psynch_cvwait" && leaf != "kevent64" && leaf != "kevent" && leaf != "__select" && leaf != "poll")
    return false;
  thread_basic_info_data_t info{};
  mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
  mach_port_t port = pthread_mach_thread_np(reinterpret_cast<pthread_t>(t->thread()->native_handle()));
  return thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) == KERN_SUCCESS &&
         info.run_state == TH_STATE_WAITING;
}

// Runs with every thread parked. Empty string = ok.
// fiber: a suspended fiber's saved registers (t and ctx null); it resumes as if its SwitchToFiber returned.
std::string AnalyzeThread(XThread* t, const arm_thread_state64_t& gp, const arm_neon_state64_t& neon, void* ctx,
                          std::pair<uint64_t, uint64_t> exe, ThreadRec& rec, bool fiber = false) {
  auto in_exe = [&](uint64_t pc) { return pc >= exe.first && pc < exe.second; };
  // Recompiled game code is extern "C"; C++ in the executable (a game's native hooks) is host code like the SDK.
  auto game = [&](uint64_t pc) { return in_exe(pc) && Sym(pc).rfind("_Z", 0) != 0; };
  struct Frame {
    uint64_t pc, sp, fp;
  };
  std::vector<Frame> f;
  int i0 = -1;
  arm_thread_state64_t at_i0{};
  arm_neon_state64_t neon_i0{};
  // Another thread's registers go in through unw_set_reg (a hand-filled unw_context_t does not unwind).
  // Syscall stubs have no unwind info, so when unwinding from pc stops at once, start at the caller (lr).
  auto walk = [&](uint64_t start_ip) {
    f.clear(), i0 = -1;
    unw_context_t uc;
    unw_cursor_t c;
    if (unw_getcontext(&uc) != 0 || unw_init_local(&c, &uc) != 0) return;
    for (int r = 0; r < 29; ++r) unw_set_reg(&c, unw_regnum_t(UNW_ARM64_X0 + r), gp.__x[r]);
    for (int r = 8; r <= 15; ++r) {
      unw_fpreg_t v;
      std::memcpy(&v, &neon.__v[r], 8);
      unw_set_fpreg(&c, unw_regnum_t(UNW_ARM64_D0 + r), v);
    }
    unw_set_reg(&c, UNW_ARM64_FP, arm_thread_state64_get_fp(gp));
    unw_set_reg(&c, UNW_ARM64_LR, arm_thread_state64_get_lr(gp));
    unw_set_reg(&c, UNW_REG_SP, arm_thread_state64_get_sp(gp));
    unw_set_reg(&c, UNW_REG_IP, start_ip);
    do {
      unw_word_t pc = 0, sp = 0, fp = 0;
      unw_get_reg(&c, UNW_REG_IP, &pc), unw_get_reg(&c, UNW_REG_SP, &sp), unw_get_reg(&c, UNW_ARM64_FP, &fp);
      f.push_back({pc, sp, fp});
      if (i0 < 0 && game(pc) && f.size() > 1) {  // the innermost game frame of a thread inside a call
        i0 = int(f.size()) - 1;
        for (int r = 19; r <= 28; ++r) {
          unw_word_t v = 0;
          unw_get_reg(&c, unw_regnum_t(UNW_ARM64_X0 + r), &v);
          at_i0.__x[r] = v;
        }
        for (int r = 8; r <= 15; ++r) {
          unw_fpreg_t v = 0;
          unw_get_fpreg(&c, unw_regnum_t(UNW_ARM64_D0 + r), &v);
          std::memcpy(&neon_i0.__v[r], &v, 8);
        }
      }
    } while (f.size() < 4096 && unw_step(&c) > 0);
  };
  walk(arm_thread_state64_get_pc(gp));
  if (f.size() <= 1 && !game(arm_thread_state64_get_pc(gp))) walk(arm_thread_state64_get_lr(gp));
  if (f.empty()) return "unwind failed";
  if (game(f[0].pc)) i0 = 0;
  if (i0 < 0) return "no game frames (pc in " + Sym(f[0].pc) + ")";
  bool leaf = true;
  for (int i = 0; i < i0; ++i) leaf = leaf && InSystemLib(f[i].pc);
  if (fiber) {
    // The fiber that switches to it has already loaded its guest registers; SwitchToFiber does nothing after.
    if (i0 <= 0) return "fiber not inside a call";
    rec.blocked = 3;
    rec.gp = at_i0;
    rec.neon = neon_i0;
    arm_thread_state64_set_fp(rec.gp, f[i0].fp);
    arm_thread_state64_set_sp(rec.gp, f[i0].sp);
    arm_thread_state64_set_pc_fptr(rec.gp, reinterpret_cast<void*>(f[i0].pc));
    rec.gp.__x[0] = 0;
    rec.gp.__cpsr = gp.__cpsr;
  } else if (i0 > 0 && !leaf) {
    std::string callee = Sym(f[i0 - 1].pc - 1);
    // A native hook (C++ in the executable) is called like a guest function, (ctx, base); restarting it is
    // only safe while it waits, so the thread must be stopped inside a system library.
    // shortcut: assumes such hooks only wait and then do their work; a hook with effects before its wait
    // would repeat them.
    const bool native = in_exe(f[i0 - 1].pc) && InSystemLib(f[0].pc);
    // C++ (not a (ctx, base) hook) that the game's own native code calls with C++ arguments, e.g. BO2's job waits.
    const bool cxx_wait = native && callee.rfind("_Z", 0) == 0;
    if (callee.find("HostToGuestFunction") == std::string::npos && !native)
      return "inside SDK code " + callee + " (called from " + Sym(f[i0].pc) + ")";
    uint64_t bl = f[i0].pc - 4;
    if ((*reinterpret_cast<const uint32_t*>(bl) & 0xFC000000u) != 0x94000000u) return "import not called by bl";
    if (!cxx_wait && !StillWaiting(t, gp)) return "wait in transit at " + Sym(arm_thread_state64_get_pc(gp));
    rec.blocked = 1;
    rec.gp = at_i0;
    rec.neon = neon_i0;
    arm_thread_state64_set_fp(rec.gp, f[i0].fp);
    arm_thread_state64_set_sp(rec.gp, f[i0].sp);
    if (cxx_wait) {
      // Its arguments are gone (caller-saved), so resume just after the call as if the wait returned true:
      // a spurious wake, which a condition wait's callers re-check.
      // shortcut: assumes native waits return bool/int "woken" and tolerate spurious wakes (true for BO2's
      // bo2zm waits); a native call that is not such a wait needs its own rule.
      rec.blocked = 2;
      rec.gp.__x[0] = 1;
      arm_thread_state64_set_pc_fptr(rec.gp, reinterpret_cast<void*>(f[i0].pc));
    } else {
      rec.gp.__x[0] = reinterpret_cast<uint64_t>(ctx);
      rec.gp.__x[1] = reinterpret_cast<uint64_t>(Runtime::instance()->memory()->virtual_membase());
      arm_thread_state64_set_pc_fptr(rec.gp, reinterpret_cast<void*>(bl));
    }
    rec.gp.__cpsr = gp.__cpsr;
  } else {
    rec.gp = gp;  // running game code (or a system leaf it called): the exact state
    rec.neon = neon;
  }
  size_t i1 = size_t(i0);
  while (i1 + 1 < f.size() && in_exe(f[i1 + 1].pc)) ++i1;
  if (i1 + 1 >= f.size() || (!fiber && Sym(f[i1 + 1].pc - 1).find("XThread7Execute") == std::string::npos))
    return "game frames not entered from XThread::Execute (" + Sym(f[std::min(i1 + 1, f.size() - 1)].pc - 1) + ")";
  for (size_t i = size_t(i0); i <= i1; ++i)
    if (!game(f[i].pc)) return "host code between game frames (" + Sym(f[i].pc) + ")";
  rec.record = f[i1].fp;
  if (rec.record < (arm_thread_state64_get_sp(rec.gp) & ~15ull) || rec.record + 16 > f[i1 + 1].sp)
    return "oldest frame record outside the game frames";
  if (reinterpret_cast<const uint64_t*>(rec.record)[1] != f[i1 + 1].pc) return "oldest frame record not found";
  rec.handle = t ? t->handle() : 0;
  rec.band_top = f[i1 + 1].sp;
  rec.band_lo = (arm_thread_state64_get_sp(rec.gp) - 256) & ~15ull;  // + red zone
  rec.band.assign(reinterpret_cast<const uint8_t*>(rec.band_lo), reinterpret_cast<const uint8_t*>(rec.band_top));
  if (ctx) {
    rec.ctx = reinterpret_cast<uint64_t>(ctx);
    rec.ctx_bytes.assign(static_cast<const uint8_t*>(ctx), static_cast<const uint8_t*>(ctx) + sizeof(::PPCContext));
  }
  return "";
}

struct Parked {
  std::atomic<bool> ready{false};
  mach_port_t port = 0;
  uint64_t fp = 0;
  std::pair<uint64_t, uint64_t> tls{0, 0};
};
std::vector<std::unique_ptr<Parked>> g_parked;
std::mutex g_parked_m;
}  // namespace

extern "C" [[noreturn]] void rex_sp_landing() {
  auto* t = XThread::GetCurrentThread();
  t->Exit(int(t->thread_state()->context()->r3.u32));
  std::abort();
}

namespace {
// Moves every 8-byte word that points into an old range by that range's delta.
struct Reloc {
  struct R {
    uint64_t lo, hi;
    int64_t delta;
  };
  std::vector<R> r;
  uint64_t guard_old = 0, guard_new = 0;  // stack-protector canary: random per process, saved in every guarded frame
  uint64_t operator()(uint64_t v) const {
    if (guard_old && v == guard_old) return guard_new;
    for (auto& x : r)
      if (v >= x.lo && v < x.hi) return uint64_t(int64_t(v) + x.delta);
    return v;
  }
};
}  // namespace

namespace {
// DIAG: REX_SAVEPOINT_PEEK=83AF71D4,...: the handle stored at each guest address and, for an event, its state.
void DiagPeek(const char* when) {
  const char* list = std::getenv("REX_SAVEPOINT_PEEK");
  if (!list) return;
  for (const char* p = list; *p;) {
    char* end = nullptr;
    const uint32_t a = uint32_t(std::strtoul(p, &end, 16));
    if (end == p) break;
    p = *end ? end + 1 : end;
    const uint32_t h = memory::load_and_swap<uint32_t>(kernel_state()->memory()->TranslateVirtual(a));
    auto obj = kernel_state()->object_table()->LookupObject<XObject>(h);
    uint32_t type = 0xFF, state = 0xFF;
    if (obj && obj->type() == XObject::Type::Event) static_cast<XEvent*>(obj.get())->Query(&type, &state);
    std::fprintf(stderr, "[savepoint] %s peek %08X = %08X obj=%d event type %u state %u\n", when, a, h,
                 obj ? int(obj->type()) : -1, type, state);
  }
}

// DIAG: what a blocked thread waits on (r3 = the handle or KEVENT the wait import got) and its state.
void DiagWaitObject(const char* when, uint32_t thread, const ::PPCContext* ctx) {
  if (!std::getenv("REX_SAVEPOINT_DIAG")) return;
  const uint32_t h = ctx->r3.u32;
  auto obj = kernel_state()->object_table()->LookupObject<XObject>(h);
  uint32_t type = 0xFF, state = 0xFF;
  if (obj && obj->type() == XObject::Type::Event) static_cast<XEvent*>(obj.get())->Query(&type, &state);
  std::fprintf(stderr,
               "[savepoint] %s wait thread %08X r3=%08X r4=%08X r5=%08X obj=%d event type %u state %u primary %08X "
               "body %08X lr %08X\n",
               when, thread, h, ctx->r4.u32, ctx->r5.u32, obj ? int(obj->type()) : -1, type, state,
               obj ? obj->handle() : 0u, obj ? obj->guest_object() : 0u, uint32_t(ctx->lr));
  // Guest call chain: each frame's back chain at [r1], the caller's saved LR at [back chain - 8].
  auto* mem = kernel_state()->memory();
  std::string chain;
  uint32_t sp = ctx->r1.u32;
  auto readable = [&](uint32_t a) {
    auto* heap = mem->LookupHeap(a);
    memory::HeapAllocationInfo info{};
    return heap && heap->QueryRegionInfo(a & ~0xFFFu, &info) && (info.state & memory::kMemoryAllocationCommit) &&
           (info.protect & memory::kMemoryProtectRead);
  };
  for (int i = 0; i < 10 && readable(sp); ++i) {
    uint32_t back = memory::load_and_swap<uint32_t>(mem->TranslateVirtual(sp));
    if (back <= sp || !readable(back - 8)) break;
    chain += fmt::format(" {:08X}", memory::load_and_swap<uint32_t>(mem->TranslateVirtual(back - 8)));
    sp = back;
  }
  std::fprintf(stderr, "[savepoint]   chain %08X:%s\n", thread, chain.c_str());
}

// REX_SAVEPOINT_VERIFY=1: page hashes of live guest memory, written beside the file at save and compared
// at load just before the threads resume. Finds memory a fresh process does not get back.
std::vector<std::pair<uint64_t, uint64_t>> PageHashes(memory::Memory* mem) {
  std::vector<std::pair<uint64_t, uint64_t>> out;  // {space<<32 | address, hash}
  auto skip = mem->FunctionTableRanges();
  auto walk = [&](memory::BaseHeap* heap, uint64_t space, uint8_t* membase) {
    if (!heap) return;
    uint64_t a = heap->heap_base(), end = uint64_t(heap->heap_base()) + heap->heap_size();
    while (a < end) {
      memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(a), &info)) break;
      uint64_t size = info.region_size ? info.region_size : heap->page_size();
      if ((info.state & memory::kMemoryAllocationCommit) && (info.protect & memory::kMemoryProtectRead)) {
        for (uint64_t p = a; p < a + size; p += 4096) {
          bool table = false;
          for (auto [tb, ts] : skip) table |= space == 0 && p >= tb && p < uint64_t(tb) + ts;
          if (table) continue;
          const uint8_t* d = membase + p;
          uint64_t h = 1469598103934665603ull;
          for (int i = 0; i < 4096; i += 8) {
            uint64_t w;
            std::memcpy(&w, d + i, 8);
            h = (h ^ w) * 1099511628211ull;
          }
          out.push_back({space << 32 | p, h});
        }
      }
      a += size;
    }
  };
  for (uint32_t b : {0x00000000u, 0x40000000u, 0x80000000u, 0x90000000u})
    walk(mem->LookupHeap(b), 0, mem->virtual_membase());
  walk(mem->GetPhysicalHeap(), 1, mem->physical_membase());
  return out;
}

void CompareHashes(const std::string& file, memory::Memory* mem) {
  FILE* f = std::fopen(file.c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "[savepoint] verify: no %s\n", file.c_str());
    return;
  }
  std::vector<std::pair<uint64_t, uint64_t>> saved;
  std::pair<uint64_t, uint64_t> e;
  while (std::fread(&e, sizeof e, 1, f) == 1) saved.push_back(e);
  std::fclose(f);
  auto now = PageHashes(mem);
  std::unordered_map<uint64_t, uint64_t> m(now.begin(), now.end());
  uint32_t diff[2] = {0, 0}, missing[2] = {0, 0}, shown = 0;
  for (auto& [k, h] : saved) {
    auto it = m.find(k);
    uint32_t sp = uint32_t(k >> 32);
    if (it == m.end()) ++missing[sp];
    else if (it->second != h) {
      ++diff[sp];
      if (sp == 0 && shown++ < 40) std::fprintf(stderr, "[savepoint] verify: virtual page %08X differs\n", uint32_t(k));
    }
  }
  std::fprintf(stderr,
               "[savepoint] verify: %zu saved pages, %zu now; differ virtual %u physical %u; missing virtual %u "
               "physical %u\n",
               saved.size(), now.size(), diff[0], diff[1], missing[0], missing[1]);
}
}  // namespace

void NoteGuestThreadStart(uint32_t handle) {
  auto block = ExeTlsBlock();
  std::lock_guard<std::mutex> l(g_tls_m);
  g_tls[handle] = block;
}

void SetGraphicsFileHook(IGraphicsSystem* graphics, GraphicsFileHook hook) {
  g_gfx_hook.store(hook, std::memory_order_release);
  g_gfx.store(graphics, std::memory_order_release);
}

std::string WriteSavepointFile(const std::string& path, int frame) {
  auto t0 = Clock::now();
  auto* gfx = g_gfx.load(std::memory_order_acquire);
  auto hook = g_gfx_hook.load(std::memory_order_acquire);
  if (!gfx || !hook) return "error no GPU file hook (Metal only)";
  uint8_t uuid[16];
  if (!MainUuid(uuid)) return "error no LC_UUID";
  auto* ks = Runtime::instance()->kernel_state();
  const auto exe = ExeRange();
  std::vector<ThreadRec> threads;
  std::vector<uint8_t> kernel(32u << 20);
  std::vector<std::string> module_paths;
  std::vector<FiberRec> fibers;
  size_t kernel_len = 0;
  uint64_t ticks = 0, time_base = 0;
  std::string why;
  int tries = 0;
  g_capture_parked_hook = [&] {
    threads.clear(), why.clear();
    if (std::getenv("REX_SAVEPOINT_VERIFY")) {
      auto hs = PageHashes(Runtime::instance()->memory());
      if (FILE* f = std::fopen((path + ".sum").c_str(), "wb")) {
        std::fwrite(hs.data(), sizeof hs[0], hs.size(), f);
        std::fclose(f);
      }
    }
    ticks = chrono::Clock::QueryGuestTickCount(), time_base = chrono::Clock::guest_system_time_base();
    VisitInProcessThreads([&](XThread* t, const arm_thread_state64_t& gp, const arm_neon_state64_t& neon, void* ctx) {
      if (!t->is_guest_thread() || !why.empty()) return;
      ThreadRec rec;
      std::string e = AnalyzeThread(t, gp, neon, ctx, exe, rec);
      if (e.empty() && rec.blocked == 1) DiagWaitObject("save", t->handle(), static_cast<const ::PPCContext*>(ctx));
      {
        std::lock_guard<std::mutex> l(g_tls_m);
        auto it = g_tls.find(t->handle());
        if (it != g_tls.end()) rec.tls_lo = it->second.first, rec.tls_hi = it->second.second;
      }
      // A thread stopped mid game code, or in a native wait whose frame spilled executable pointers, resumes
      // with host pointers the load cannot move (BO2's main then called an unregistered function, twice):
      // retry for a moment where every thread sits in a kernel wait; accept the rest after ~1.2 s.
      if (e.empty() && rec.blocked != 1 && tries < 60) e = rec.blocked ? "woken in a native wait" : "running in game code";
      if (!e.empty()) why = fmt::format("thread {:08X}: {}", t->handle(), e);
      threads.push_back(std::move(rec));
    });
    fibers.clear();
    for (auto& [addr, info] : ks->fibers()) {
      if (!why.empty()) break;
      FiberRec fr;
      fr.addr = addr, fr.uid = info.uid, fr.stack_base = info.guest_stack_base, fr.stack_bottom = info.guest_stack_bottom;
      fr.start = info.start_address;
      if (info.is_thread_fiber) {
        fr.kind = kThreadFiber;
        for (auto& th : ks->object_table()->GetObjectsByType<XThread>(XObject::Type::Thread))
          if (th->main_fiber() == info.host_fiber) fr.owner = th->handle();
        if (!fr.owner) why = fmt::format("fiber {:08X}: thread fiber without its thread", addr);
      } else if (!info.host_fiber->started()) {
        fr.kind = kFreshFiber, fr.host_stack = info.host_fiber->stack_size();
      } else {
        // A fiber running on a thread fails that thread's analysis (its frames do not start at XThread::Execute).
        fr.kind = kSuspendedFiber, fr.host_stack = info.host_fiber->stack_size();
        auto* mc = info.host_fiber->ucontext()->uc_mcontext;
        std::string e = AnalyzeThread(nullptr, mc->__ss, mc->__ns, nullptr, exe, fr.rec, true);
        if (!e.empty()) why = fmt::format("fiber {:08X}: {}", addr, e);
      }
      fibers.push_back(std::move(fr));
    }
    // Kernel objects: the table's handle counts, then guest threads, then the rest (type, length, Save bytes).
    stream::ByteStream bs(kernel.data(), kernel.size());
    DiagPeek("save");
    if (std::getenv("REX_SAVEPOINT_DIAG"))  // how each module image's pages look to the region scan
      for (auto& m : ks->object_table()->GetObjectsByType<XModule>(XObject::Type::Module)) {
        if (m->is_host_object()) continue;
        auto* xm = static_cast<UserModule*>(m.get())->xex_module();
        for (uint32_t a : {xm ? xm->base_address() : 0u, 0x832DF034u}) {
          memory::HeapAllocationInfo info{};
          auto* heap = Runtime::instance()->memory()->LookupHeap(a);
          if (heap && heap->QueryRegionInfo(a, &info))
            std::fprintf(stderr, "[savepoint] image %s %08X: heap %08X state %x protect %x size %x alloc %08X\n",
                         m->name().c_str(), a, heap->heap_base(), info.state, info.protect, info.region_size,
                         info.allocation_base);
        }
      }
    ks->content_manager()->SaveOpenContent(&bs);  // before objects: restored files reopen on these devices
    // Procedure thunks: guest addresses handed out for native exports, which the game keeps in its globals.
    auto pools = ks->function_dispatcher()->SaveThunks();
    bs.Write<uint32_t>(uint32_t(pools.size()));
    for (auto& pool : pools) {
      bs.Write<uint32_t>(pool.code_base), bs.Write<uint32_t>(pool.next_thunk_address);
      bs.Write<uint32_t>(uint32_t(pool.free_thunks.size()));
      for (uint32_t a : pool.free_thunks) bs.Write<uint32_t>(a);
      bs.Write<uint32_t>(uint32_t(pool.thunks.size()));
      for (auto& [addr, func] : pool.thunks) {
        std::string image;
        uint64_t offset = 0;
        if (!FuncToFile(func, image, offset) && why.empty()) why = fmt::format("thunk {:08X}: native not in an image", addr);
        bs.Write<uint32_t>(addr), bs.Write(std::string_view(image)), bs.Write<uint64_t>(offset);
      }
    }
    ks->object_table()->Save(&bs);
    for (auto& m : ks->object_table()->GetObjectsByType<XModule>(XObject::Type::Module))
      if (!m->is_host_object()) module_paths.push_back(m->path());
    std::vector<object_ref<XObject>> order;
    for (int pass = 0; pass < 2; ++pass)
      for (auto& o : ks->object_table()->GetAllObjects())
        if (!o->is_host_object() && (o->type() == XObject::Type::Thread) == (pass == 0)) order.push_back(o);
    bs.Write<uint32_t>(uint32_t(order.size()));
    for (auto& o : order) {
      bs.Write<uint32_t>(uint32_t(o->type()));
      size_t len_at = bs.offset();
      bs.Write<uint32_t>(0);
      if (!o->Save(&bs) && why.empty()) why = fmt::format("object {:08X} type {} cannot be saved", o->handle(), int(o->type()));
      uint32_t len = uint32_t(bs.offset() - len_at - 4);
      std::memcpy(kernel.data() + len_at, &len, 4);
    }
    // Audio render clients (host state): their worker calls the game's audio callback every frame.
    auto* audio = dynamic_cast<rex::audio::AudioSystem*>(Runtime::instance()->audio_system());
    bs.Write<uint32_t>(audio ? 1 : 0);
    if (audio) audio->Save(&bs);
    kernel_len = bs.offset();
  };
  SavepointStats st;
  for (; tries < 80; ++tries) {
    st = CaptureInProcessSavepoint();
    if (!st.ok || why.empty()) break;
    REXSYS_INFO("[savepoint] file save retry: {}", why);
    ::usleep(20000);
  }
  g_capture_parked_hook = nullptr;
  if (!st.ok) return "error capture failed: " + st.error;
  if (!why.empty()) return "error " + why;
  if (std::getenv("REX_SAVEPOINT_OBJS")) LogObjects("save");
  auto t1 = Clock::now();
  // Same command-processor safe point as the capture: nothing has been drawn since.
  std::vector<uint8_t> gpu_state, gpu_pages;
  if (!hook(gfx, 0, gpu_state, gpu_pages)) return "error GPU save failed (drain timed out?)";
  struct Page {
    uint32_t address, size;
    const uint8_t* data;
  };
  std::vector<Page> pages;
  for (size_t at = 0; at + 8 <= gpu_pages.size();) {
    Page pg;
    std::memcpy(&pg.address, &gpu_pages[at], 4);
    std::memcpy(&pg.size, &gpu_pages[at + 4], 4);
    pg.data = &gpu_pages[at + 8];
    at += 8 + pg.size;
    pages.push_back(pg);
  }

  std::error_code ec;  // runq live save puts files in <session>/savepoints/, which may not exist yet
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return "error open " + tmp + ": " + std::strerror(errno);
  Out out{fd};
  out.Put(kMagic, 8);
  out.U32(kVersion);
  out.Put(uuid, 16);
  out.U32(uint32_t(frame));
  // Module paths before memory: the loader loads the DLLs its boot lacks first (heap + function tables).
  out.U32(uint32_t(module_paths.size()));
  for (auto& m : module_paths) out.Str(m);
  auto* mem = Runtime::instance()->memory();
  std::vector<std::pair<memory::BaseHeap*, const memory::BaseHeap::RewindState*>> heaps;
  struct Region {
    uint32_t space, base, size, protect;
    const uint8_t* data;
  };
  std::vector<Region> regions;
  VisitInProcessSnapshot(
      [&](memory::BaseHeap* h, const memory::BaseHeap::RewindState& s) { heaps.push_back({h, &s}); },
      [&](uint32_t space, uint32_t base, uint32_t size, uint32_t protect, const uint8_t* data) {
        regions.push_back({space, base, size, protect, data});
      });
  out.U32(uint32_t(heaps.size()));
  for (auto& [h, s] : heaps) {
    out.U32(h->heap_base());
    out.U32(h == mem->GetPhysicalHeap() ? 1 : 0);
    out.U32(s->unreserved_page_count);
    out.U32(uint32_t(s->pages.size()));
    out.Put(s->pages.data(), s->pages.size() * 8);
  }
  out.U32(uint32_t(regions.size()));
  uint64_t overlaid = 0;
  std::vector<uint8_t> patched;
  for (auto& r : regions) {
    out.U32(r.space), out.U32(r.base), out.U32(r.size), out.U32(r.protect);
    const uint8_t* data = r.data;
    if (r.space == 1) {
      for (auto& pg : pages) {
        uint64_t lo = std::max<uint64_t>(pg.address, r.base);
        uint64_t hi = std::min<uint64_t>(uint64_t(pg.address) + pg.size, uint64_t(r.base) + r.size);
        if (lo >= hi) continue;
        if (data != patched.data()) patched.assign(r.data, r.data + r.size), data = patched.data();
        std::memcpy(patched.data() + (lo - r.base), pg.data + (lo - pg.address), hi - lo);
        overlaid += hi - lo;
      }
    }
    out.Put(data, r.size);
  }
  out.U32(uint32_t(gpu_state.size()));
  out.Put(gpu_state.data(), gpu_state.size());
  // head[6]: queued DPCs (the list head is host state, the entries guest memory; a DPC marked queued but not
  // on the list is never inserted again).
  uint64_t head[7] = {reinterpret_cast<uint64_t>(mem->virtual_membase()), exe.first, exe.second, ticks, time_base,
                      uint64_t(__stack_chk_guard), kernel_state()->dpc_list()->head()};
  out.Put(head, sizeof head);
  out.U32(uint32_t(kernel_len));
  out.Put(kernel.data(), kernel_len);
  auto put_rec = [&](const ThreadRec& t) {
    out.U32(t.handle), out.U32(t.blocked);
    out.Put(&t.gp, sizeof t.gp), out.Put(&t.neon, sizeof t.neon);
    uint64_t w[6] = {t.band_lo, t.band_top, t.record, t.ctx, t.tls_lo, t.tls_hi};
    out.Put(w, sizeof w);
    out.U32(uint32_t(t.ctx_bytes.size())), out.Put(t.ctx_bytes.data(), t.ctx_bytes.size());
    out.U32(uint32_t(t.band.size())), out.Put(t.band.data(), t.band.size());
  };
  out.U32(uint32_t(threads.size()));
  for (auto& t : threads) put_rec(t);
  out.U32(uint32_t(fibers.size()));
  for (auto& f : fibers) {
    uint32_t w[7] = {f.addr, f.uid, f.stack_base, f.stack_bottom, f.start, f.owner, f.kind};
    out.Put(w, sizeof w), out.Put(&f.host_stack, 8);
    put_rec(f.rec);
  }
  out.U32(kEnd);
  bool ok = out.ok && ::close(fd) == 0 && ::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) {
    ::unlink(tmp.c_str());
    return "error write failed: " + path;
  }
  auto t2 = Clock::now();
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "ok file frame=%d mb=%llu gpu_pages_kb=%llu capture_ms=%.1f write_ms=%.1f threads=%u guest_threads=%zu "
                "kernel_kb=%zu tries=%d", frame,
                (unsigned long long)(out.bytes >> 20), (unsigned long long)(overlaid >> 10), Ms(t0, t1),
                Ms(t1, t2), st.threads_parked, threads.size(), kernel_len >> 10, tries + 1);
  REXSYS_INFO("[savepoint] wrote {}: {}", path, buf);
  return buf;
}

namespace {
// REX_SAVEPOINT_DIAG: report host faults outside the guest arena (the SDK's handler owns the arena) with the
// faulting thread's frames, then hand the signal on. Crash reports are throttled after many crashes.
struct sigaction g_old_segv, g_old_bus, g_old_abrt, g_old_trap, g_old_ill;
uint64_t g_arena_lo = 0, g_arena_hi = 0;
void DiagPrint(int sig, uint64_t addr, ucontext_t* uc) {
  auto& ss = uc->uc_mcontext->__ss;
  std::fprintf(stderr, "[savepoint] FAULT sig %d addr %llx thread %08X pc %llx lr %llx sp %llx fp %llx\n", sig,
               (unsigned long long)addr, XThread::IsInThread() ? XThread::GetCurrentThreadHandle() : 0u,
               (unsigned long long)arm_thread_state64_get_pc(ss), (unsigned long long)arm_thread_state64_get_lr(ss),
               (unsigned long long)arm_thread_state64_get_sp(ss), (unsigned long long)arm_thread_state64_get_fp(ss));
  for (int r = 19; r <= 28; ++r) std::fprintf(stderr, " x%d=%llx", r, (unsigned long long)ss.__x[r]);
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
  // Walk only inside this thread's own stack: a bad fp must not fault again in here (that kills silently).
  const uint64_t hi = uint64_t(pthread_get_stackaddr_np(pthread_self()));
  const uint64_t lo = hi - pthread_get_stacksize_np(pthread_self());
  uint64_t pcs[24] = {arm_thread_state64_get_pc(ss), arm_thread_state64_get_lr(ss)};
  int n = 2;
  for (uint64_t fp = arm_thread_state64_get_fp(ss); n < 24 && fp >= lo && fp + 16 <= hi && !(fp & 7);
       fp = *reinterpret_cast<uint64_t*>(fp))
    pcs[n++] = reinterpret_cast<uint64_t*>(fp)[1];
  for (int i = 0; i < n; ++i) {
    Dl_info di{};
    dladdr(reinterpret_cast<void*>(pcs[i]), &di);
    std::fprintf(stderr, "[savepoint]   %d %llx %s+%llx\n", i, (unsigned long long)pcs[i],
                 di.dli_sname ? di.dli_sname : "?",
                 (unsigned long long)(pcs[i] - uint64_t(di.dli_saddr ? di.dli_saddr : 0)));
  }
  std::fflush(stderr);
}
void DiagFault(int sig, siginfo_t* si, void* uc_v) {
  auto* uc = static_cast<ucontext_t*>(uc_v);
  uint64_t addr = uint64_t(si->si_addr);
  const bool arena = addr >= g_arena_lo && addr < g_arena_hi;
  if (!arena) DiagPrint(sig, addr, uc);
  auto& old = sig == SIGBUS    ? g_old_bus
              : sig == SIGABRT ? g_old_abrt
              : sig == SIGTRAP ? g_old_trap
              : sig == SIGILL  ? g_old_ill
                               : g_old_segv;
  if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(sig, si, uc_v);
  else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) old.sa_handler(sig);
  else {
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }
  // The SDK handler resets the signal to the default when it could not resolve the fault (the retry then dies).
  struct sigaction now{};
  sigaction(sig, nullptr, &now);
  if (arena && !(now.sa_flags & SA_SIGINFO) && now.sa_handler == SIG_DFL) DiagPrint(sig, addr, uc);
}
void InstallDiagFaultHandler(memory::Memory* mem) {
  g_arena_lo = uint64_t(mem->virtual_membase());
  g_arena_hi = g_arena_lo + 0x200000000ull;
  struct sigaction sa{};
  sa.sa_sigaction = DiagFault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, &g_old_segv);
  sigaction(SIGBUS, &sa, &g_old_bus);
  sigaction(SIGABRT, &sa, &g_old_abrt);
  sigaction(SIGTRAP, &sa, &g_old_trap);  // guest breakpoints / bad restored code
  sigaction(SIGILL, &sa, &g_old_ill);
}
}  // namespace

bool LoadSavepointFile(const std::string& path) {
  auto t0 = Clock::now();
  int fd = ::open(path.c_str(), O_RDONLY);
  struct stat sb{};
  if (fd < 0 || ::fstat(fd, &sb) != 0) {
    SPLOAD_ERR("[savepoint] load: cannot open {}", path);
    if (fd >= 0) ::close(fd);
    return false;
  }
  void* map = ::mmap(nullptr, size_t(sb.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED) {
    SPLOAD_ERR("[savepoint] load: mmap failed for {}", path);
    return false;
  }
  struct Unmap {
    void* p;
    size_t n;
    ~Unmap() { ::munmap(p, n); }
  } unmap{map, size_t(sb.st_size)};
  In in{static_cast<const uint8_t*>(map), static_cast<const uint8_t*>(map) + sb.st_size};
  auto* magic = in.Take(8);
  if (!magic || std::memcmp(magic, kMagic, 8) != 0 || in.U32() != kVersion) {
    SPLOAD_ERR("[savepoint] load: {} is not a version {} save point file", path, kVersion);
    return false;
  }
  uint8_t uuid[16];
  auto* file_uuid = in.Take(16);
  if (!MainUuid(uuid) || !file_uuid || std::memcmp(uuid, file_uuid, 16) != 0) {
    SPLOAD_ERR("[savepoint] load: {} was saved by a different executable build; refusing", path);
    return false;
  }
  uint32_t frame = in.U32();
  std::vector<std::pair<std::string, object_ref<XModule>>> loaded_dlls;  // loaded here; records fix handles
  for (uint32_t n = in.U32(); n && in.ok; --n) {
    auto m = in.Str();
    auto* kst = Runtime::instance()->kernel_state();
    // Same path() string as one of this boot's modules (GetModule's path resolve misses the main xex).
    auto mods = kst->object_table()->GetObjectsByType<XModule>(XObject::Type::Module);
    if (std::any_of(mods.begin(), mods.end(), [&](auto& x) { return x->path() == m; })) continue;
    auto dll = kst->LoadUserModule(m, false);
    if (!dll) {
      SPLOAD_ERR("[savepoint] load: module {} did not load", m);
      return false;
    }
    REXSYS_INFO("[savepoint] loaded module {} (a DLL the game had loaded)", m);
    loaded_dlls.emplace_back(m, object_ref<XModule>(dll.release()));
  }

  if (std::getenv("REX_SAVEPOINT_OBJS")) LogObjects("load-boot");
  auto* mem = Runtime::instance()->memory();
  if (std::getenv("REX_SAVEPOINT_DIAG")) InstallDiagFaultHandler(mem);
  // This process's function tables hold its own host pointers; keep them over the restored bytes.
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> tables;
  for (auto [base, size] : mem->FunctionTableRanges()) {
    auto* p = mem->TranslateVirtual<uint8_t*>(base);
    tables.push_back({base, {p, p + size}});
  }

  uint32_t heap_count = in.U32();
  std::vector<memory::BaseHeap*> view_heaps;
  for (uint32_t i = 0; in.ok && i < heap_count; ++i) {
    uint32_t base = in.U32(), physical = in.U32();
    memory::BaseHeap::RewindState state;
    state.unreserved_page_count = in.U32();
    uint32_t n = in.U32();
    auto* pages = in.Take(size_t(n) * 8);
    auto* heap = HeapFor(mem, base, physical != 0);
    if (!pages || !heap || heap->heap_base() != base ||
        heap->CaptureRewindState().pages.size() != n) {
      SPLOAD_ERR("[savepoint] load: heap {:08X} layout differs from this process", base);
      return false;
    }
    state.pages.resize(n);
    std::memcpy(state.pages.data(), pages, size_t(n) * 8);
    heap->Rewind(state);
    if (!physical && base >= 0xA0000000u) view_heaps.push_back(heap);
  }

  uint32_t region_count = in.U32();
  uint64_t bytes = 0;
  struct Saved {
    uint32_t base, size;
    const uint8_t* data;
  };
  std::vector<Saved> saved_virtual;  // REX_SAVEPOINT_VERIFY: compared word by word before the threads resume
  // REX_SAVEPOINT_VERIFY: guest words that changed since the regions were restored, at each load step.
  auto verify_words = [&](const char* step) {
    DiagPeek(step);
    if (!std::getenv("REX_SAVEPOINT_VERIFY")) return;
    uint32_t shown = 0, words = 0;
    for (auto& r : saved_virtual) {
      const uint8_t* now = mem->virtual_membase() + r.base;
      for (uint32_t o = 0; o + 4 <= r.size; o += 4) {
        if (std::memcmp(now + o, r.data + o, 4) == 0) continue;
        bool table = false;
        for (auto& [tb, tv] : tables) table |= r.base + o >= tb && r.base + o < tb + tv.size();
        if (table) continue;
        ++words;
        uint32_t a, b;
        std::memcpy(&a, r.data + o, 4), std::memcpy(&b, now + o, 4);
        if (shown++ < 12)
          std::fprintf(stderr, "[savepoint] verify %s: %08X file %08X now %08X\n", step, r.base + o,
                       __builtin_bswap32(a), __builtin_bswap32(b));
      }
    }
    std::fprintf(stderr, "[savepoint] verify %s: %u virtual words changed since the regions were restored\n", step,
                 words);
  };
  // This boot's vblank interrupt would run the game's handler on half-restored memory (Forza bugchecks there):
  // held from here until the function returns, after the restored threads are let go.
  struct HoldInterrupts {
    std::vector<uint8_t> none;
    bool Call(int op) {
      auto* g = g_gfx.load(std::memory_order_acquire);
      auto h = g_gfx_hook.load(std::memory_order_acquire);
      return g && h && h(g, op, none, none);
    }
    HoldInterrupts() { Call(2); }
    ~HoldInterrupts() { Call(3); }
  } hold_interrupts;
  const uint32_t rw = memory::kMemoryProtectRead | memory::kMemoryProtectWrite;
  for (uint32_t i = 0; in.ok && i < region_count; ++i) {
    uint32_t space = in.U32(), base = in.U32(), size = in.U32(), protect = in.U32();
    auto* data = in.Take(size);
    auto* heap = space == 0 ? mem->LookupHeap(base) : mem->GetPhysicalHeap();
    if (!data || !heap || !heap->Protect(base, size, rw)) {
      SPLOAD_ERR("[savepoint] load: region {}:{:08X} +{:X} cannot be restored", space, base, size);
      return false;
    }
    uint8_t* host = (space == 0 ? mem->virtual_membase() : mem->physical_membase()) + base;
    std::memcpy(host, data, size);
    if (space == 0) saved_virtual.push_back({base, size, data});
    heap->Protect(base, size, protect);
    bytes += size;
  }
  for (auto* heap : view_heaps) SyncHeapAccess(heap);
  verify_words("after memory");
  for (auto& [base, saved] : tables) {
    std::memcpy(mem->TranslateVirtual<uint8_t*>(base), saved.data(), saved.size());
  }

  std::vector<uint8_t> gpu_state(in.U32()), unused;
  if (auto* s = in.Take(gpu_state.size())) std::memcpy(gpu_state.data(), s, gpu_state.size());
  uint64_t head[7] = {};
  if (auto* h = in.Take(sizeof head)) std::memcpy(head, h, sizeof head);
  std::vector<uint8_t> kernel(in.U32());
  if (auto* k = in.Take(kernel.size())) std::memcpy(kernel.data(), k, kernel.size());
  auto get_rec = [&](ThreadRec& t) {
    t.handle = in.U32(), t.blocked = in.U32();
    if (auto* g = in.Take(sizeof t.gp)) std::memcpy(&t.gp, g, sizeof t.gp);
    if (auto* n = in.Take(sizeof t.neon)) std::memcpy(&t.neon, n, sizeof t.neon);
    uint64_t w[6] = {};
    if (auto* x = in.Take(sizeof w)) std::memcpy(w, x, sizeof w);
    t.band_lo = w[0], t.band_top = w[1], t.record = w[2], t.ctx = w[3], t.tls_lo = w[4], t.tls_hi = w[5];
    t.ctx_bytes.resize(in.U32());
    if (auto* c = in.Take(t.ctx_bytes.size())) std::memcpy(t.ctx_bytes.data(), c, t.ctx_bytes.size());
    t.band.resize(in.U32());
    if (auto* b = in.Take(t.band.size())) std::memcpy(t.band.data(), b, t.band.size());
  };
  std::vector<ThreadRec> threads(in.U32());
  for (auto& t : threads)
    if (in.ok) get_rec(t);
  std::vector<FiberRec> fibers(in.ok ? in.U32() : 0);
  for (auto& f : fibers) {
    uint32_t w[7] = {};
    if (auto* x = in.Take(sizeof w)) std::memcpy(w, x, sizeof w);
    f.addr = w[0], f.uid = w[1], f.stack_base = w[2], f.stack_bottom = w[3], f.start = w[4], f.owner = w[5], f.kind = w[6];
    if (auto* x = in.Take(8)) std::memcpy(&f.host_stack, x, 8);
    if (in.ok) get_rec(f.rec);
  }
  if (!in.ok || in.U32() != kEnd) {
    SPLOAD_ERR("[savepoint] load: {} is truncated", path);
    return false;
  }
  for (auto& t : threads) {
    if (t.ctx_bytes.size() != sizeof(::PPCContext)) {
      SPLOAD_ERR("[savepoint] load: thread {:08X} context size differs", t.handle);
      return false;
    }
  }
  chrono::Clock::RestoreGuestClock(head[3], head[4]);
  kernel_state()->dpc_list()->set_head(uint32_t(head[6]));
  if (std::getenv("REX_SAVEPOINT_DIAG")) std::fprintf(stderr, "[savepoint] dpc list head %08X\n", uint32_t(head[6]));
  input::RebaseVirtualPadScripts();  // pad scripts count from the load, as they count from boot otherwise

  // Kernel objects, keeping the ones this boot already made (same handles: boot is deterministic).
  auto* ks = Runtime::instance()->kernel_state();
  {
    std::lock_guard<std::mutex> l(g_parked_m);
    g_parked.clear();
    for (size_t i = 0; i < threads.size(); ++i) g_parked.push_back(std::make_unique<Parked>());
  }
  XThread::restore_body_ = [threads_handles = [&] {
    std::vector<uint32_t> h;
    for (auto& t : threads) h.push_back(t.handle);
    return h;
  }()](XThread* t) {
    Parked* pk = nullptr;
    {
      std::lock_guard<std::mutex> l(g_parked_m);
      for (size_t i = 0; i < threads_handles.size(); ++i)
        if (threads_handles[i] == t->handle()) pk = g_parked[i].get();
    }
    if (!pk) return;
    if (std::getenv("REX_SAVEPOINT_DIAG")) {  // fault reports even when the restored sp is bad
      stack_t alt{std::malloc(1 << 16), 1 << 16, 0};
      sigaltstack(&alt, nullptr);
    }
    pk->port = mach_thread_self();
    pk->tls = ExeTlsBlock();
    pk->fp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    pk->ready.store(true, std::memory_order_release);
    for (;;) __asm__ volatile("yield");  // user mode, no syscall: the loader replaces this state
  };
  stream::ByteStream kbs(kernel.data(), kernel.size());
  ks->content_manager()->RestoreOpenContent(&kbs);
  std::vector<runtime::FunctionDispatcher::ThunkPool> pools(kbs.Read<uint32_t>());
  for (auto& pool : pools) {
    pool.code_base = kbs.Read<uint32_t>(), pool.next_thunk_address = kbs.Read<uint32_t>();
    pool.free_thunks.resize(kbs.Read<uint32_t>());
    for (auto& a : pool.free_thunks) a = kbs.Read<uint32_t>();
    pool.thunks.resize(kbs.Read<uint32_t>());
    for (auto& [addr, func] : pool.thunks) {
      addr = kbs.Read<uint32_t>();
      auto image = kbs.Read<std::string>();
      func = FuncFromFile(image, kbs.Read<uint64_t>());
      if (!func) {
        SPLOAD_ERR("[savepoint] load: thunk {:08X}: image {} not loaded", addr, image);
        return false;
      }
    }
  }
  if (!ks->function_dispatcher()->RestoreThunks(pools)) {
    SPLOAD_ERR("[savepoint] load: procedure thunks not restored (a module pool is missing)");
    return false;
  }
  ks->object_table()->Restore(&kbs);
  uint32_t objects = kbs.Read<uint32_t>(), restored_objects = 0;
  for (uint32_t i = 0; i < objects; ++i) {
    auto type = XObject::Type(kbs.Read<uint32_t>());
    uint32_t len = kbs.Read<uint32_t>();
    size_t start = kbs.offset();
    uint32_t handle = 0;
    std::memcpy(&handle, kernel.data() + start + 12, 4);  // SaveObject: alloc flag, guest ptr, count, handles
    // Modules: boot ones are this process's already (same executable, checked above); DLLs loaded above take
    // the saved handles. Records start with the module signature and path, not the handle.
    if (type == XObject::Type::Module) {
      kbs.Read<uint32_t>();
      auto m = kbs.Read<std::string>();
      auto dll = std::find_if(loaded_dlls.begin(), loaded_dlls.end(), [&](auto& d) { return d.first == m; });
      if (dll != loaded_dlls.end()) {
        if (!dll->second->RestoreLoaded(&kbs)) {
          SPLOAD_ERR("[savepoint] load: module {} did not take its saved handles", m);
          return false;
        }
      }
      kbs.set_offset(start + len);
      continue;
    }
    if (
        (type != XObject::Type::Thread && ks->object_table()->LookupObject<XObject>(handle))) {
      kbs.set_offset(start + len);
      continue;
    }
    if (!XObject::Restore(ks, type, &kbs)) {
      SPLOAD_ERR("[savepoint] load: object {:08X} type {} did not restore", handle, int(type));
      return false;
    }
    kbs.set_offset(start + len);
    ++restored_objects;
  }
  if (kbs.Read<uint32_t>()) {
    auto* audio = dynamic_cast<rex::audio::AudioSystem*>(Runtime::instance()->audio_system());
    if (!audio || !audio->Restore(&kbs)) {
      SPLOAD_ERR("[savepoint] load: audio clients restore failed");
      return false;
    }
  }

  verify_words("after objects");
  auto t1 = Clock::now();
  auto* gfx = g_gfx.load(std::memory_order_acquire);
  auto hook = g_gfx_hook.load(std::memory_order_acquire);
  if (!gfx || !hook || !hook(gfx, 1, gpu_state, unused)) {
    SPLOAD_ERR("[savepoint] load: GPU state restore failed");
    return false;
  }

  verify_words("before threads");

  // Threads: start each restored host thread, wait for it to park, then move its band in and let it go.
  const auto exe = ExeRange();
  const uint64_t membase = reinterpret_cast<uint64_t>(mem->virtual_membase());
  // Fibers before the threads that switch to them; this boot's own fibers are dropped (leaked).
  // shortcut: a fiber band gets no thread-local mapping (no game keeps native C++ frames on a fiber yet).
  // Recompiled code keeps pointers to the executable's globals in callee-saved registers and spills them in its
  // frames (Forza faulted writing through one into the moved image). shortcut: a 64-bit guest value that happens
  // to fall in the old image's writable segments moves too; narrow ranges, so accepted until it bites.
  Reloc exe_data;
  for (auto [lo, hi] : ExeDataRanges())
    exe_data.r.push_back({lo - (exe.first - head[1]), hi - (exe.first - head[1]), int64_t(exe.first - head[1])});
  Reloc ctxs;  // a fiber band holds the context pointer of the thread that ran it
  for (auto& rec : threads)
    if (auto t = ks->object_table()->LookupObject<XThread>(rec.handle))
      ctxs.r.push_back({rec.ctx, rec.ctx + sizeof(::PPCContext),
                        int64_t(reinterpret_cast<uint64_t>(t->thread_state()->context()) - rec.ctx)});
  ks->fibers().clear();
  for (auto& f : fibers) {
    FiberInfo info{nullptr, f.uid, f.addr, f.stack_base, f.stack_bottom, f.kind == kThreadFiber, f.start};
    if (f.kind == kThreadFiber) {
      if (auto t = ks->object_table()->LookupObject<XThread>(f.owner))
        info.host_fiber = rex::thread::Fiber::ForThread(), t->set_main_fiber(info.host_fiber);
    } else if (f.kind == kFreshFiber) {
      info.host_fiber = kernel::crt::RecreateGuestFiber(ks, f.start, f.addr, f.host_stack);
    } else if (auto* hf = rex::thread::Fiber::Create(f.host_stack, [](void*) { std::abort(); }, nullptr);
               hf && f.rec.band.size() + 65536 + 4096 <= hf->stack_size()) {
      // Same band move as a thread's (below), into the new fiber's stack, then its saved context points there.
      const auto& rec = f.rec;
      const uint64_t new_top = (reinterpret_cast<uint64_t>(hf->stack_data()) + hf->stack_size() - 65536) & ~15ull;
      const int64_t sd = int64_t(new_top - rec.band_top);
      Reloc rl = ctxs;
      rl.r.insert(rl.r.end(), exe_data.r.begin(), exe_data.r.end());
      rl.r.push_back({rec.band_lo, rec.band_top, sd});
      rl.r.push_back({head[0], head[0] + kArenaSize, int64_t(membase - head[0])});
      rl.guard_old = head[5], rl.guard_new = uint64_t(__stack_chk_guard);
      Reloc code{{{head[1], head[2], int64_t(exe.first - head[1])}}};
      uint8_t* dst = reinterpret_cast<uint8_t*>(new_top - rec.band.size());
      std::memcpy(dst, rec.band.data(), rec.band.size());
      for (size_t o = 0; o + 8 <= rec.band.size(); o += 8) {
        uint64_t v;
        std::memcpy(&v, dst + o, 8);
        v = rl(v);
        std::memcpy(dst + o, &v, 8);
      }
      bool ok = true;
      for (uint64_t r = rl(arm_thread_state64_get_fp(rec.gp)); ok && r && r != rec.record + sd;) {
        ok = r >= new_top - rec.band.size() && r + 16 <= new_top;
        if (!ok) break;
        auto* fr = reinterpret_cast<uint64_t*>(r);
        fr[1] = code(fr[1]);
        r = fr[0];
      }
      auto* record = reinterpret_cast<uint64_t*>(rec.record + sd);
      record[0] = 0;
      record[1] = reinterpret_cast<uint64_t>(&rex_sp_landing);
      // swapcontext into it restores x19-x28, fp, sp, d8-d15 and continues at the saved return address.
      auto* mc = hf->ucontext()->uc_mcontext;
      for (int r = 19; r <= 28; ++r) mc->__ss.__x[r] = rl(rec.gp.__x[r]);
      arm_thread_state64_set_fp(mc->__ss, rl(arm_thread_state64_get_fp(rec.gp)));
      arm_thread_state64_set_sp(mc->__ss, rl(arm_thread_state64_get_sp(rec.gp)));
      void* resume = reinterpret_cast<void*>(code(arm_thread_state64_get_pc(rec.gp)));
      arm_thread_state64_set_lr_fptr(mc->__ss, resume);
      arm_thread_state64_set_pc_fptr(mc->__ss, resume);
      for (int r = 8; r <= 15; ++r) mc->__ns.__v[r] = rec.neon.__v[r];
      if (ok) info.host_fiber = hf;
    }
    if (!info.host_fiber) {
      SPLOAD_ERR("[savepoint] load: fiber {:08X} (kind {}) not rebuilt", f.addr, f.kind);
      return false;
    }
    ks->RegisterFiber(f.addr, info);
  }
  if (std::getenv("REX_SAVEPOINT_DIAG")) std::fprintf(stderr, "[savepoint] %zu fibers rebuilt\n", fibers.size());
  for (size_t i = 0; i < threads.size(); ++i) {
    auto& rec = threads[i];
    auto t = ks->object_table()->LookupObject<XThread>(rec.handle);
    if (!t || !t->thread()) {
      SPLOAD_ERR("[savepoint] load: thread {:08X} was not recreated", rec.handle);
      return false;
    }
    t->thread()->Resume();
    auto until = Clock::now() + std::chrono::seconds(5);
    while (!g_parked[i]->ready.load(std::memory_order_acquire) && Clock::now() < until) ::usleep(200);
    if (!g_parked[i]->ready.load(std::memory_order_acquire)) {
      SPLOAD_ERR("[savepoint] load: thread {:08X} did not park", rec.handle);
      return false;
    }
    auto* ctx = t->thread_state()->context();
    std::memcpy(ctx, rec.ctx_bytes.data(), sizeof(::PPCContext));
    if (rec.blocked == 1) DiagWaitObject("load", rec.handle, ctx);
    const uint64_t new_top = (g_parked[i]->fp - 65536) & ~15ull;
    const int64_t sd = int64_t(new_top - rec.band_top);
    // Not the executable range: guest 64-bit sums (sign-extended lis + zero-extended lwz) land at 0x1_0xxxxxxx,
    // right where the executable is mapped. Code addresses are moved only where they are known to be code.
    const auto ntls = g_parked[i]->tls;
    if (rec.tls_lo && rec.tls_hi - rec.tls_lo != ntls.second - ntls.first) {
      SPLOAD_ERR("[savepoint] load: thread {:08X} thread-local block size differs", rec.handle);
      return false;
    }
    // shortcut: the new thread's thread-locals start fresh (pointers into the block are moved, values not
    // copied); copy the saved block too if a game keeps state there that matters across the load.
    Reloc rl{{{rec.band_lo, rec.band_top, sd},
              {rec.tls_lo, rec.tls_hi, int64_t(ntls.first - rec.tls_lo)},
              {head[0], head[0] + kArenaSize, int64_t(membase - head[0])},
              {rec.ctx, rec.ctx + sizeof(::PPCContext), int64_t(reinterpret_cast<uint64_t>(ctx) - rec.ctx)}}};
    rl.r.insert(rl.r.end(), exe_data.r.begin(), exe_data.r.end());
    rl.guard_old = head[5], rl.guard_new = uint64_t(__stack_chk_guard);
    Reloc code{{{head[1], head[2], int64_t(exe.first - head[1])}}};
    mach_port_t port = g_parked[i]->port;
    if (thread_suspend(port) != KERN_SUCCESS) return false;
    uint8_t* dst = reinterpret_cast<uint8_t*>(new_top - rec.band.size());
    std::memcpy(dst, rec.band.data(), rec.band.size());
    for (size_t o = 0; o + 8 <= rec.band.size(); o += 8) {
      uint64_t v;
      std::memcpy(&v, dst + o, 8);
      v = rl(v);
      std::memcpy(dst + o, &v, 8);
    }
    // Return addresses: walk the frame records (now in the new band) from the innermost game frame.
    uint32_t moved_returns = 0;
    for (uint64_t r = rl(arm_thread_state64_get_fp(rec.gp)); r && r != rec.record + sd;) {
      if (r < new_top - rec.band.size() || r + 16 > new_top) {
        SPLOAD_ERR("[savepoint] load: thread {:08X} frame record {:X} outside its band", rec.handle, r);
        return false;
      }
      auto* fr = reinterpret_cast<uint64_t*>(r);
      fr[1] = code(fr[1]), ++moved_returns;
      r = fr[0];
    }
    auto* record = reinterpret_cast<uint64_t*>(rec.record + sd);  // oldest game frame returns to the landing
    record[0] = 0;
    record[1] = reinterpret_cast<uint64_t>(&rex_sp_landing);
    arm_thread_state64_t gp = rec.gp;
    for (int r = 0; r < 29; ++r) gp.__x[r] = rl(gp.__x[r]);
    // Resumed inside native C++ (blocked 2): its callee-saved registers hold pointers to the executable's globals.
    // shortcut: only x19-x28 are moved, not exe pointers the native frame spilled to its stack; recompiled
    // frames never get this (their 64-bit guest sums share the 0x1_0xxxxxxx range).
    if (rec.blocked == 2)
      for (int r = 19; r <= 28; ++r) gp.__x[r] = code(gp.__x[r]);
    arm_thread_state64_set_fp(gp, rl(arm_thread_state64_get_fp(rec.gp)));
    arm_thread_state64_set_sp(gp, rl(arm_thread_state64_get_sp(rec.gp)));
    arm_thread_state64_set_lr_fptr(gp, reinterpret_cast<void*>(code(arm_thread_state64_get_lr(rec.gp))));
    arm_thread_state64_set_pc_fptr(gp, reinterpret_cast<void*>(code(arm_thread_state64_get_pc(rec.gp))));
    // Bisect switch: REX_SAVEPOINT_RESUME_ONLY=F8000030,F8000034 leaves every other restored thread parked.
    const char* only = std::getenv("REX_SAVEPOINT_RESUME_ONLY");
    char hex[16];
    std::snprintf(hex, sizeof hex, "%08X", rec.handle);
    const bool resume = !only || std::strstr(only, hex);
    if (std::getenv("REX_SAVEPOINT_DIAG")) {
      Dl_info di{};
      dladdr(reinterpret_cast<void*>(arm_thread_state64_get_pc(gp)), &di);
      std::fprintf(stderr, "[savepoint] thread %s %s pc %llx (%s) band %zu KB, %u return addresses moved%s\n", hex,
                   rec.blocked == 2 ? "woken" : rec.blocked ? "blocked" : "running", (unsigned long long)arm_thread_state64_get_pc(gp),
                   di.dli_sname ? di.dli_sname : "?", rec.band.size() >> 10, moved_returns, resume ? "" : ", left parked");
      std::fprintf(stderr, "[savepoint]   tls old %llx-%llx new %llx-%llx; x19-x28 old/new:", (unsigned long long)rec.tls_lo,
                   (unsigned long long)rec.tls_hi, (unsigned long long)ntls.first, (unsigned long long)ntls.second);
      for (int r = 19; r <= 28; ++r)
        std::fprintf(stderr, " %llx/%llx", (unsigned long long)rec.gp.__x[r], (unsigned long long)gp.__x[r]);
      std::fprintf(stderr, "\n");
    }
    if (!resume) continue;  // still suspended, spinning in its park frame when resumed
    if (thread_set_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&gp), ARM_THREAD_STATE64_COUNT) !=
            KERN_SUCCESS ||
        thread_set_state(port, ARM_NEON_STATE64, reinterpret_cast<thread_state_t>(&rec.neon),
                         ARM_NEON_STATE64_COUNT) != KERN_SUCCESS) {
      SPLOAD_ERR("[savepoint] load: thread {:08X} state not set", rec.handle);
      return false;
    }
    thread_resume(port);
    DiagPeek(hex);  // REX_SAVEPOINT_PEEK: which resumed thread changes a watched word
  }
  DiagPeek("load");
  REXSYS_INFO("[savepoint] restored {}: frame {}, {} MB in {} regions, {} objects, {} threads, memory+kernel "
              "{:.1f} ms, total {:.1f} ms",
              path, frame, bytes >> 20, region_count, restored_objects, threads.size(), Ms(t0, t1),
              Ms(t0, Clock::now()));
  if (std::getenv("REX_SAVEPOINT_DIAG")) {
    // What every guest thread is doing every 15 s for 90 s: a blocked import keeps its arguments in the context.
    std::thread([] {
      for (int s = 15; s <= 90; s += 15) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        const std::string when = fmt::format("after {} s", s);
        DiagPeek(when.c_str());
        // REX_SAVEPOINT_SAMPLE_DIR: host stacks of every thread (recompiled guest code is symbolized), 1 s sample.
        if (const char* dir = std::getenv("REX_SAVEPOINT_SAMPLE_DIR"))
          std::system(fmt::format("sample {} 1 -mayDie -file '{}/sample_{:02}.txt' >/dev/null 2>&1", getpid(), dir, s).c_str());
        for (auto& o : kernel_state()->object_table()->GetAllObjects()) {
          if (o->type() != XObject::Type::Thread) continue;
          auto* t = static_cast<XThread*>(o.get());
          if (!t->is_guest_thread() || !t->thread_state()) continue;
          DiagWaitObject(when.c_str(), t->handle(), t->thread_state()->context());
        }
      }
    }).detach();
  }
  return true;
}

int LoadSavepointAtLaunch() {
  const std::string path = REXCVAR_GET(load_savepoint);
  if (path.empty()) return 0;
  return LoadSavepointFile(path) ? 1 : -1;
}

}  // namespace rex::system
