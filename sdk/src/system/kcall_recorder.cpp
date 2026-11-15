/**
 * @file        kcall_recorder.cpp
 * @brief       Kernel-call flight recorder (see rex/kcall_recorder.h).
 *
 * Layout: each thread that makes a recorded kernel call owns a ring of fixed-size entries. A
 * thread only ever writes its own ring; dumps read every ring without locks, using a per-entry
 * sequence number to skip entries that are being rewritten. Rings and site descriptors are never
 * freed, so a dump can run from a signal handler at any moment.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/kcall_recorder.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(std::string, log_file);
REXCVAR_DEFINE_INT32(kcall_stall_dump_s, 20, "Debug",
                     "Kernel-call recorder (REX_KCALL_RING): write a stall dump when no frame has "
                     "been presented for this many seconds. 0 = off.");

namespace rex::kcall {

uint32_t g_ring_entries = 0;

// ---------------------------------------------------------------------------------------------
// Data layout
// ---------------------------------------------------------------------------------------------

struct Entry {
  std::atomic<uint64_t> seq;  // index + 1 in the owning ring; 0 while being written
  uint64_t t_begin;
  std::atomic<uint64_t> t_end;  // 0 = still inside the call
  const Site* site;
  uint32_t args[8];
  uint32_t lr;      // guest return address
  uint32_t result;  // r3 after the call (if the export returns a value)
  uint32_t out;     // decoded out-parameter (created handle etc.), valid if has_out
  uint8_t has_out;
  char extra[51];  // decoded text: path, timeout, handle list
};
static_assert(sizeof(Entry) == 128);

// Per-export decoding rules, resolved from the export name on first use.
struct Decode {
  const Site* site;
  Decode* next;  // registry of every site seen
  bool skip;     // noisy export: only the in-flight slot is kept
  int8_t objattr_arg = -1;   // X_OBJECT_ATTRIBUTES* -> name
  int8_t ansi_arg = -1;      // X_ANSI_STRING* -> text
  int8_t timeout_arg = -1;   // be int64* (100 ns units, negative = relative, null = infinite)
  int8_t multi_count_arg = -1;
  int8_t multi_array_arg = -1;
  int8_t out_arg = -1;        // be uint32* written on success
};

struct Ring {
  Entry* entries;
  uint32_t mask;
  std::atomic<uint64_t> next{0};  // entries begun so far
  Entry hot{};                    // most recent call to a skipped (noisy) export
  uint64_t host_tid = 0;
  char name[64] = {};
  uint64_t name_refresh = 0;
  Ring* next_ring = nullptr;
};

struct HandleEv {
  std::atomic<uint64_t> seq;
  uint64_t t;
  const Site* site;  // innermost recorded export on the thread, or null for native code
  const Ring* ring;
  uint64_t host_tid;
  uint32_t handle;
  uint32_t lr;
  int32_t ref_count;
  uint8_t op;
  uint8_t type;
};

namespace {

std::atomic<Ring*> g_rings{nullptr};
std::atomic<Decode*> g_decodes{nullptr};
std::atomic<const Site*> g_swap_site{nullptr};

HandleEv* g_handle_ring = nullptr;
uint32_t g_handle_mask = 0;
std::atomic<uint64_t> g_handle_next{0};

thread_local Ring* tls_ring = nullptr;
thread_local const Site* tls_site = nullptr;
thread_local uint32_t tls_lr = 0;

uint64_t g_t0 = 0;
uint64_t g_tb_numer = 1, g_tb_denom = 1;

std::atomic<bool> g_started{false};
std::atomic<bool> g_crash_dumped{false};
std::atomic<bool> g_dump_requested{false};
std::atomic<int> g_dump_serial{0};
char g_out_dir[1024] = ".";
char g_skip_list[1024] = {};
std::atomic<const char*> g_fatal_note{nullptr};
char g_fatal_buf[512];

struct sigaction g_old_abrt, g_old_trap;

uint64_t Now() {
#if defined(__APPLE__)
  return mach_absolute_time();
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

uint64_t ToNs(uint64_t ticks) {
  return uint64_t((unsigned __int128)ticks * g_tb_numer / g_tb_denom);
}

uint64_t HostTid() {
#if defined(__APPLE__)
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return tid;
#else
  static std::atomic<uint64_t> counter{1};
  thread_local uint64_t id = counter.fetch_add(1);
  return id;
#endif
}

void RefreshName(Ring* r) {
#if defined(__APPLE__) || defined(__linux__)
  char buf[64] = {};
  if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0 && buf[0]) {
    memcpy(r->name, buf, sizeof(buf));
  }
#endif
}

// ----- environment -------------------------------------------------------------------------

uint32_t RoundPow2(uint64_t v, uint32_t lo, uint32_t hi) {
  v = std::clamp<uint64_t>(v, lo, hi);
  uint32_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

const char kDefaultSkip[] =
    "RtlEnterCriticalSection,RtlLeaveCriticalSection,RtlTryEnterCriticalSection,"
    "KeQueryPerformanceCounter,KeQueryPerformanceFrequency,KfAcquireSpinLock,KfReleaseSpinLock,"
    "KeAcquireSpinLockAtRaisedIrql,KeReleaseSpinLockFromRaisedIrql,"
    "KeTryToAcquireSpinLockAtRaisedIrql,KeRaiseIrqlToDpcLevel,KfLowerIrql,KeTlsGetValue,"
    "KeTlsSetValue,KeGetCurrentProcessType,KeQuerySystemTime,KeQueryInterruptTime,"
    "NtQuerySystemTime,KeLockL2,KeUnlockL2,XamInputGetState,XamInputGetCapabilities,"
    "RtlInitAnsiString,RtlInitUnicodeString,RtlNtStatusToDosError,RtlCompareMemoryUlong,"
    "RtlFillMemoryUlong,RtlImageXexHeaderField,KeEnterCriticalRegion,KeLeaveCriticalRegion,"
    "MmGetPhysicalAddress,MmQueryAddressProtect,MmQueryAllocationSize";

struct EnvInit {
  EnvInit() {
    const char* ring = getenv("REX_KCALL_RING");
    if (!ring || !*ring) return;
    long long n = atoll(ring);
    if (n <= 0) return;
    const char* skip = getenv("REX_KCALL_SKIP");
    snprintf(g_skip_list, sizeof(g_skip_list), ",%s,", skip ? skip : kDefaultSkip);
    const char* hring = getenv("REX_KCALL_HANDLE_RING");
    g_handle_mask = RoundPow2(hring ? atoll(hring) : 65536, 1024, 1u << 22) - 1;
    g_handle_ring = static_cast<HandleEv*>(calloc(g_handle_mask + 1, sizeof(HandleEv)));
#if defined(__APPLE__)
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_tb_numer = tb.numer;
    g_tb_denom = tb.denom;
#endif
    g_t0 = Now();
    if (g_handle_ring) g_ring_entries = RoundPow2(uint64_t(n), 64, 1u << 20);
  }
};
EnvInit g_env_init;

// ----- decoding --------------------------------------------------------------------------------

const char* BareName(const char* name) {
  return strncmp(name, "__imp__", 7) == 0 ? name + 7 : name;
}

bool NameIs(const char* bare, const char* want) { return strcmp(bare, want) == 0; }

Decode* MakeDecode(const Site& site) {
  auto* d = new Decode();
  d->site = &site;
  const char* n = BareName(site.name);
  char key[96];
  snprintf(key, sizeof(key), ",%s,", n);
  d->skip = strstr(g_skip_list, key) != nullptr;

  struct Rule {
    const char* name;
    int8_t objattr, ansi, timeout, mcount, marray, out;
  };
  static const Rule rules[] = {
      {"NtCreateFile", 2, -1, -1, -1, -1, 0},
      {"NtOpenFile", 2, -1, -1, -1, -1, 0},
      {"NtQueryFullAttributesFile", 0, -1, -1, -1, -1, -1},
      {"NtQueryDirectoryFile", -1, 7, -1, -1, -1, -1},
      {"NtOpenSymbolicLinkObject", 1, -1, -1, -1, -1, 0},
      {"NtCreateEvent", 1, -1, -1, -1, -1, 0},
      {"NtCreateSemaphore", 1, -1, -1, -1, -1, 0},
      {"NtCreateMutant", 1, -1, -1, -1, -1, 0},
      {"NtCreateTimer", 1, -1, -1, -1, -1, 0},
      {"NtCreateIoCompletion", -1, -1, -1, -1, -1, 0},
      {"ObCreateSymbolicLink", -1, 0, -1, -1, -1, -1},
      {"ObDeleteSymbolicLink", -1, 0, -1, -1, -1, -1},
      {"ExCreateThread", -1, -1, -1, -1, -1, 0},
      {"NtDuplicateObject", -1, -1, -1, -1, -1, 1},
      {"ObReferenceObjectByHandle", -1, -1, -1, -1, -1, 2},
      {"ObReferenceObjectByName", -1, 0, -1, -1, -1, 4},
      {"KeWaitForSingleObject", -1, -1, 4, -1, -1, -1},
      {"NtWaitForSingleObjectEx", -1, -1, 3, -1, -1, -1},
      {"KeWaitForMultipleObjects", -1, -1, 6, 0, 1, -1},
      {"NtWaitForMultipleObjectsEx", -1, -1, 5, 0, 1, -1},
      {"NtSignalAndWaitForSingleObjectEx", -1, -1, 4, -1, -1, -1},
      {"KeDelayExecutionThread", -1, -1, 2, -1, -1, -1},
      {"NtSetTimerEx", -1, -1, 1, -1, -1, -1},
      {"KeSetTimer", -1, -1, 1, -1, -1, -1},
      {"KeSetTimerEx", -1, -1, 1, -1, -1, -1},
      {"NtRemoveIoCompletion", -1, -1, 4, -1, -1, -1},
  };
  for (const Rule& r : rules) {
    if (NameIs(n, r.name)) {
      d->objattr_arg = r.objattr;
      d->ansi_arg = r.ansi;
      d->timeout_arg = r.timeout;
      d->multi_count_arg = r.mcount;
      d->multi_array_arg = r.marray;
      d->out_arg = r.out;
      break;
    }
  }
  if (NameIs(n, "VdSwap")) g_swap_site.store(&site, std::memory_order_release);

  Decode* head = g_decodes.load(std::memory_order_relaxed);
  do {
    d->next = head;
  } while (!g_decodes.compare_exchange_weak(head, d, std::memory_order_release,
                                            std::memory_order_relaxed));
  return d;
}

const Decode* Resolve(Site& site) {
  static std::atomic_flag lock = ATOMIC_FLAG_INIT;
  while (lock.test_and_set(std::memory_order_acquire)) {
  }
  const Decode* d = site.decode.load(std::memory_order_acquire);
  if (!d) {
    d = MakeDecode(site);
    site.decode.store(d, std::memory_order_release);
  }
  lock.clear(std::memory_order_release);
  return d;
}

// Guest pointer -> host pointer, or null when the address is clearly not usable.
template <typename T>
const T* Guest(uint32_t addr) {
  if (addr < 0x10000 || addr >= 0xFFFF0000u) return nullptr;
  auto* ks = REX_KERNEL_STATE();
  if (!ks || !ks->memory()) return nullptr;
  return ks->memory()->TranslateVirtual<const T*>(addr);
}

uint32_t Be32(const void* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
uint16_t Be16(const void* p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}

// Copies an X_ANSI_STRING (be16 length, be16 max, be32 buffer) into dst, keeping the tail.
void CopyAnsi(uint32_t str_addr, char* dst, size_t cap) {
  const uint8_t* s = Guest<uint8_t>(str_addr);
  if (!s) return;
  uint16_t len = Be16(s);
  const char* buf = Guest<char>(Be32(s + 4));
  if (!buf || !len) return;
  size_t keep = std::min<size_t>(len, cap - 1);
  const char* from = buf + (len - keep);
  for (size_t i = 0; i < keep; ++i) {
    char c = from[i];
    dst[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
  }
  dst[keep] = 0;
  if (keep < len && keep > 0) dst[0] = '~';
}

void DecodeBegin(const Decode& d, Entry& e) {
  e.extra[0] = 0;
  if (d.objattr_arg >= 0) {
    const uint8_t* oa = Guest<uint8_t>(e.args[d.objattr_arg]);
    if (oa) CopyAnsi(Be32(oa + 4), e.extra, sizeof(e.extra));
  } else if (d.ansi_arg >= 0) {
    CopyAnsi(e.args[d.ansi_arg], e.extra, sizeof(e.extra));
  }
  size_t used = strlen(e.extra);
  if (d.multi_count_arg >= 0 && d.multi_array_arg >= 0) {
    uint32_t count = e.args[d.multi_count_arg];
    const uint8_t* arr = Guest<uint8_t>(e.args[d.multi_array_arg]);
    if (arr && count) {
      used += snprintf(e.extra + used, sizeof(e.extra) - used, "[");
      for (uint32_t i = 0; i < count && i < 4 && used < sizeof(e.extra) - 10; ++i) {
        used += snprintf(e.extra + used, sizeof(e.extra) - used, i ? " %08X" : "%08X",
                         Be32(arr + i * 4));
      }
      if (used < sizeof(e.extra) - 4) {
        used += snprintf(e.extra + used, sizeof(e.extra) - used, count > 4 ? "..]" : "]");
      }
    }
  }
  if (d.timeout_arg >= 0 && used < sizeof(e.extra) - 16) {
    uint32_t p = e.args[d.timeout_arg];
    if (!p) {
      snprintf(e.extra + used, sizeof(e.extra) - used, "%st=inf", used ? " " : "");
    } else if (const uint8_t* t = Guest<uint8_t>(p)) {
      uint64_t raw;
      memcpy(&raw, t, 8);
      int64_t v = int64_t(__builtin_bswap64(raw));
      if (v <= 0) {
        snprintf(e.extra + used, sizeof(e.extra) - used, "%st=%lldus", used ? " " : "",
                 (long long)(-v / 10));
      } else {
        snprintf(e.extra + used, sizeof(e.extra) - used, "%st=abs", used ? " " : "");
      }
    }
  }
}

// ----- dumping -----------------------------------------------------------------------------

// Small buffered writer that only uses write(2) and snprintf on stack/static storage.
struct Out {
  int fd = -1;
  char buf[1 << 16];
  size_t n = 0;
  void Flush() {
    size_t off = 0;
    while (off < n) {
      ssize_t w = write(fd, buf + off, n - off);
      if (w <= 0) {
        if (w < 0 && errno == EINTR) continue;
        break;
      }
      off += size_t(w);
    }
    n = 0;
  }
  __attribute__((format(printf, 2, 3))) void Printf(const char* fmt, ...) {
    if (n > sizeof(buf) - 1024) Flush();
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (w > 0) n += std::min<size_t>(size_t(w), sizeof(buf) - n - 1);
  }
};

Out g_out;  // dumps are serialised by g_dump_lock
std::atomic_flag g_dump_lock = ATOMIC_FLAG_INIT;

void PrintTime(Out& o, uint64_t t) {
  uint64_t ns = t > g_t0 ? ToNs(t - g_t0) : 0;
  o.Printf("%5llu.%06llu", (unsigned long long)(ns / 1000000000ull),
           (unsigned long long)(ns / 1000 % 1000000));
}

const char* ThreadName(const Ring* r) { return r && r->name[0] ? r->name : "?"; }

// Copies an entry if it is stable; returns false if it is being rewritten or empty.
bool Snapshot(const Entry& src, Entry& dst, uint64_t want_seq) {
  uint64_t s1 = src.seq.load(std::memory_order_acquire);
  if (s1 == 0 || (want_seq && s1 != want_seq)) return false;
  dst.t_begin = src.t_begin;
  dst.t_end.store(src.t_end.load(std::memory_order_relaxed), std::memory_order_relaxed);
  dst.site = src.site;
  memcpy(dst.args, src.args, sizeof(dst.args));
  dst.lr = src.lr;
  dst.result = src.result;
  dst.out = src.out;
  dst.has_out = src.has_out;
  memcpy(dst.extra, src.extra, sizeof(dst.extra));
  dst.extra[sizeof(dst.extra) - 1] = 0;
  std::atomic_thread_fence(std::memory_order_acquire);
  uint64_t s2 = src.seq.load(std::memory_order_relaxed);
  dst.seq.store(s1, std::memory_order_relaxed);
  return s1 == s2 && dst.site != nullptr;
}

void PrintEntry(Out& o, const Ring* r, const Entry& e, bool with_thread) {
  PrintTime(o, e.t_begin);
  if (with_thread) o.Printf(" %-24.24s", ThreadName(r));
  const Site* s = e.site;
  o.Printf(" %s(", BareName(s->name));
  for (int i = 0; i < s->arity && i < 8; ++i) o.Printf(i ? ", %08X" : "%08X", e.args[i]);
  o.Printf(")");
  uint64_t t_end = e.t_end.load(std::memory_order_relaxed);
  if (!t_end) {
    o.Printf(" -> (in call)");
  } else {
    if (s->returns) o.Printf(" -> %08X", e.result);
    uint64_t us = ToNs(t_end - e.t_begin) / 1000;
    if (us >= 1000) o.Printf(" %llums", (unsigned long long)(us / 1000));
    else if (us) o.Printf(" %lluus", (unsigned long long)us);
  }
  if (e.has_out) o.Printf(" out=%08X", e.out);
  o.Printf(" lr=%08X", e.lr);
  if (e.extra[0]) o.Printf(" \"%s\"", e.extra);
  o.Printf("\n");
}

// Number of entries in ring r that began at or after t (entries are in time order per ring).
struct Span {
  uint64_t first, last;  // seq range [first, last)
};
Span ValidSpan(const Ring* r) {
  uint64_t last = r->next.load(std::memory_order_acquire);
  uint64_t size = uint64_t(r->mask) + 1;
  uint64_t first = last > size ? last - size + 1 : 0;  // +1: the oldest slot may be rewriting
  return {first, last};
}

uint64_t FirstAtOrAfter(const Ring* r, Span sp, uint64_t t) {
  uint64_t lo = sp.first, hi = sp.last;
  while (lo < hi) {
    uint64_t mid = lo + (hi - lo) / 2;
    const Entry& e = r->entries[mid & r->mask];
    if (e.t_begin < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

constexpr int kMaxRings = 512;

void WriteDump(Out& o, const char* reason, int sig) {
  uint64_t now = Now();
  o.Printf("kernel-call recorder dump\nreason: %s", reason);
  if (sig) o.Printf(" (signal %d)", sig);
  o.Printf("\npid: %d\n", int(getpid()));
  if (const char* note = g_fatal_note.load(std::memory_order_acquire)) {
    o.Printf("fatal: %s\n", note);
  }
  o.Printf("dumping thread: %s tid=%llu\n", tls_ring ? ThreadName(tls_ring) : "(no ring)",
           (unsigned long long)HostTid());
  o.Printf("time since recorder start: ");
  PrintTime(o, now);
  const Site* swap = g_swap_site.load(std::memory_order_acquire);
  o.Printf(" s\nframes presented (VdSwap calls): %llu\n",
           (unsigned long long)(swap ? swap->calls.load(std::memory_order_relaxed) : 0));
  o.Printf("ring: %u entries per thread, handle ring %u\nnot recorded (in-flight only): %s\n",
           g_ring_entries, g_handle_mask + 1, g_skip_list);
  o.Printf("columns: seconds  thread  export(args r3..) -> r3  duration  out=created handle  "
           "lr=guest caller  \"decoded\"\n");

  // Static, not on the stack: the dump may run on a crashing thread's stack (dumps are serialised).
  static const Ring* rings[kMaxRings];
  static Span spans[kMaxRings];
  static uint64_t cursor[kMaxRings];
  int nr = 0;
  for (Ring* r = g_rings.load(std::memory_order_acquire); r && nr < kMaxRings; r = r->next_ring) {
    rings[nr++] = r;
  }

  // 1. Calls still in progress on every thread (blocked waits, the crashing call).
  o.Printf("\n== calls in progress ==\n");
  for (int i = 0; i < nr; ++i) {
    const Ring* r = rings[i];
    Span sp = ValidSpan(r);
    Entry e;
    int shown = 0;
    for (uint64_t s = sp.last; s > sp.first && sp.last - s < 64; --s) {
      const Entry& src = r->entries[(s - 1) & r->mask];
      if (src.t_end.load(std::memory_order_relaxed) == 0 && Snapshot(src, e, s)) {
        o.Printf("[%s tid=%llu] ", ThreadName(r), (unsigned long long)r->host_tid);
        PrintEntry(o, r, e, false);
        ++shown;
      }
    }
    if (r->hot.site && r->hot.t_end.load(std::memory_order_relaxed) == 0 &&
        Snapshot(r->hot, e, 0)) {
      o.Printf("[%s tid=%llu] (unrecorded) ", ThreadName(r), (unsigned long long)r->host_tid);
      PrintEntry(o, r, e, false);
    }
    (void)shown;
  }

  // 2. Merged timeline of the newest calls across all threads.
  const uint64_t kMerged = 40000;
  uint64_t lo = g_t0, hi = now + 1;
  for (int i = 0; i < nr; ++i) spans[i] = ValidSpan(rings[i]);
  for (int it = 0; it < 64 && lo < hi; ++it) {
    uint64_t mid = lo + (hi - lo) / 2;
    uint64_t count = 0;
    for (int i = 0; i < nr; ++i) count += spans[i].last - FirstAtOrAfter(rings[i], spans[i], mid);
    if (count > kMerged) lo = mid + 1;
    else hi = mid;
  }
  uint64_t start_t = ~0ull;
  for (int i = 0; i < nr; ++i) {
    cursor[i] = FirstAtOrAfter(rings[i], spans[i], lo);
    // Earliest time each thread still covers, so readers know where the record starts.
    if (spans[i].first < spans[i].last) {
      start_t = std::min(start_t, rings[i]->entries[spans[i].first & rings[i]->mask].t_begin);
    }
  }
  o.Printf("\n== merged timeline (newest %llu calls, all threads) ==\n",
           (unsigned long long)kMerged);
  for (;;) {
    int best = -1;
    uint64_t best_t = ~0ull;
    for (int i = 0; i < nr; ++i) {
      if (cursor[i] >= spans[i].last) continue;
      uint64_t t = rings[i]->entries[cursor[i] & rings[i]->mask].t_begin;
      if (t < best_t) best_t = t, best = i;
    }
    if (best < 0) break;
    const Ring* r = rings[best];
    uint64_t s = cursor[best]++;
    Entry e;
    if (Snapshot(r->entries[s & r->mask], e, s + 1)) PrintEntry(o, r, e, true);
  }
  (void)start_t;

  // 3. Handle lifetimes.
  o.Printf("\n== handle table events (A=add R=remove +=retain -=release; newest last) ==\n");
  static const char* kTypes[] = {"Undefined", "Enumerator", "Event",  "File",    "IOCompletion",
                                 "Module",    "Mutant",     "Notify", "Semaphore", "Session",
                                 "Socket",    "SymLink",    "Thread", "Timer"};
  uint64_t hn = g_handle_next.load(std::memory_order_acquire);
  uint64_t hsize = uint64_t(g_handle_mask) + 1;
  for (uint64_t s = hn > hsize ? hn - hsize + 1 : 0; s < hn; ++s) {
    const HandleEv& h = g_handle_ring[s & g_handle_mask];
    if (h.seq.load(std::memory_order_acquire) != s + 1) continue;
    PrintTime(o, h.t);
    o.Printf(" %-24.24s %c %08X %-12s ref=%d", h.ring ? ThreadName(h.ring) : "(native)", h.op,
             h.handle, h.type < sizeof(kTypes) / sizeof(kTypes[0]) ? kTypes[h.type] : "?",
             h.ref_count);
    if (h.site) o.Printf(" in %s lr=%08X", BareName(h.site->name), h.lr);
    o.Printf("\n");
  }

  // 4. Every thread's own ring, oldest first.
  o.Printf("\n== per-thread rings ==\n");
  for (int i = 0; i < nr; ++i) {
    const Ring* r = rings[i];
    Span sp = ValidSpan(r);
    o.Printf("\n-- thread %s tid=%llu: %llu calls recorded, showing %llu --\n", ThreadName(r),
             (unsigned long long)r->host_tid, (unsigned long long)sp.last,
             (unsigned long long)(sp.last - sp.first));
    for (uint64_t s = sp.first; s < sp.last; ++s) {
      Entry e;
      if (Snapshot(r->entries[s & r->mask], e, s + 1)) PrintEntry(o, r, e, false);
    }
  }

  // 5. Call counts per export.
  o.Printf("\n== calls per export (since start) ==\n");
  for (Decode* d = g_decodes.load(std::memory_order_acquire); d; d = d->next) {
    o.Printf("%12llu %s%s\n", (unsigned long long)d->site->calls.load(std::memory_order_relaxed),
             BareName(d->site->name), d->skip ? " (not recorded)" : "");
  }
  o.Printf("\nend of dump\n");
}

const char* DoDump(const char* reason, int sig, bool wait_for_lock) {
  static char path[1200];
  // A crash during another dump waits up to 2 s for it; a request or stall dump just skips.
  for (int tries = 0; g_dump_lock.test_and_set(std::memory_order_acquire); ++tries) {
    if (!wait_for_lock || tries >= 200) return nullptr;
    usleep(10000);
  }
  int serial = g_dump_serial.fetch_add(1);
  snprintf(path, sizeof(path), "%s/kcall-%s-%d-%d.txt", g_out_dir, reason, int(getpid()), serial);
  g_out.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  const char* result = nullptr;
  if (g_out.fd >= 0) {
    g_out.n = 0;
    WriteDump(g_out, reason, sig);
    g_out.Flush();
    close(g_out.fd);
    g_out.fd = -1;
    result = path;
    char msg[1300];
    int len = snprintf(msg, sizeof(msg), "[kcall] wrote %s\n", path);
    if (len > 0) (void)!write(STDERR_FILENO, msg, size_t(std::min(len, int(sizeof(msg) - 1))));
  }
  g_dump_lock.clear(std::memory_order_release);
  return result;
}

const char* SignalReason(int sig) {
  switch (sig) {
    case SIGABRT: return "abort";
    case SIGTRAP: return "trap";
    case SIGILL: return "sigill";
    case SIGSEGV: return "segv";
    case SIGBUS: return "sigbus";
    case SIGFPE: return "sigfpe";
    default: return "signal";
  }
}

void FatalSignal(int sig, struct sigaction* old) {
  DumpFromSignal(sig);
  // Put the previous handler back; the trap re-executes (or abort re-raises) and the process
  // dies the normal way, so the system crash report is still written.
  sigaction(sig, old, nullptr);
  if (sig == SIGABRT) raise(SIGABRT);
}

void OnAbort(int sig) { FatalSignal(sig, &g_old_abrt); }
void OnTrap(int sig) { FatalSignal(sig, &g_old_trap); }
void OnInfo(int) { g_dump_requested.store(true, std::memory_order_relaxed); }

#if defined(__APPLE__) || defined(__linux__)
#define KCALL_DUMP_TRIGGERS 1
#endif

#if KCALL_DUMP_TRIGGERS
void Watchdog() {
#if defined(__APPLE__)
  pthread_setname_np("kcall watchdog");
#else
  pthread_setname_np(pthread_self(), "kcall watchdog");
#endif
  char request[1200];
  snprintf(request, sizeof(request), "%s/kcall-dump-request", g_out_dir);
  uint64_t last_frames = 0;
  int still_s = 0;
  bool stall_dumped = false;
  int stall_dumps = 0;
  for (;;) {
    sleep(1);
    if (g_crash_dumped.load(std::memory_order_relaxed)) continue;
    if (g_dump_requested.exchange(false) || access(request, F_OK) == 0) {
      unlink(request);
      DoDump("request", 0, false);
    }
    const Site* swap = g_swap_site.load(std::memory_order_acquire);
    int limit = REXCVAR_GET(kcall_stall_dump_s);
    if (!swap || limit <= 0) continue;
    uint64_t frames = swap->calls.load(std::memory_order_relaxed);
    if (frames != last_frames) {
      last_frames = frames;
      still_s = 0;
      stall_dumped = false;
      continue;
    }
    if (++still_s >= limit && !stall_dumped && stall_dumps < 8) {
      stall_dumped = true;
      ++stall_dumps;
      DoDump("stall", 0, false);
    }
  }
}
#endif  // KCALL_DUMP_TRIGGERS

void Start() {
  // Output folder: REX_KCALL_DIR, else beside the log file, else the working directory.
  const char* dir = getenv("REX_KCALL_DIR");
  std::string log = REXCVAR_GET(log_file);
  if (dir && *dir) {
    snprintf(g_out_dir, sizeof(g_out_dir), "%s", dir);
  } else if (!log.empty()) {
    auto slash = log.find_last_of('/');
    snprintf(g_out_dir, sizeof(g_out_dir), "%s",
             slash == std::string::npos ? "." : log.substr(0, slash).c_str());
  } else if (getcwd(g_out_dir, sizeof(g_out_dir)) == nullptr) {
    snprintf(g_out_dir, sizeof(g_out_dir), ".");
  }

#if KCALL_DUMP_TRIGGERS
  struct sigaction sa = {};
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_ONSTACK;
  sa.sa_handler = OnAbort;
  sigaction(SIGABRT, &sa, &g_old_abrt);
  sa.sa_handler = OnTrap;
  sigaction(SIGTRAP, &sa, &g_old_trap);
#ifdef SIGINFO
  sa.sa_handler = OnInfo;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGINFO, &sa, nullptr);
#endif
  std::thread(Watchdog).detach();
#endif
  char msg[1200];
  int len = snprintf(msg, sizeof(msg),
                     "[kcall] recorder on: %u entries/thread, dumps go to %s\n", g_ring_entries,
                     g_out_dir);
  if (len > 0) (void)!write(STDERR_FILENO, msg, size_t(std::min(len, int(sizeof(msg) - 1))));
}

void EnsureStarted() {
  static std::atomic_flag starting = ATOMIC_FLAG_INIT;
  if (g_started.load(std::memory_order_acquire)) return;
  if (starting.test_and_set(std::memory_order_acq_rel)) return;  // another thread is starting
  Start();
  g_started.store(true, std::memory_order_release);
}

Ring* NewRing() {
  Ring* r = new Ring();
  r->entries = static_cast<Entry*>(calloc(g_ring_entries, sizeof(Entry)));
  r->mask = g_ring_entries - 1;
  r->host_tid = HostTid();
  RefreshName(r);
  Ring* head = g_rings.load(std::memory_order_relaxed);
  do {
    r->next_ring = head;
  } while (!g_rings.compare_exchange_weak(head, r, std::memory_order_release,
                                          std::memory_order_relaxed));
  tls_ring = r;
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Hot path
// ---------------------------------------------------------------------------------------------

Entry* Begin(Site& site, PPCContext& ctx, uint8_t* /*base*/, uint64_t* seq_out) {
  if (__builtin_expect(!g_started.load(std::memory_order_relaxed), 0)) EnsureStarted();
  site.calls.fetch_add(1, std::memory_order_relaxed);
  const Decode* d = site.decode.load(std::memory_order_acquire);
  if (__builtin_expect(!d, 0)) d = Resolve(site);
  Ring* r = tls_ring;
  if (__builtin_expect(!r, 0)) r = NewRing();
  uint32_t lr = uint32_t(ctx.lr);

  if (d->skip) {
    Entry& h = r->hot;
    h.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    h.site = &site;
    h.t_begin = Now();
    h.t_end.store(0, std::memory_order_relaxed);
    const PPCRegister* regs = &ctx.r3;
    for (int i = 0; i < 4; ++i) h.args[i] = regs[i].u32;
    h.lr = lr;
    h.has_out = 0;
    h.extra[0] = 0;
    h.seq.store(~0ull, std::memory_order_release);
    *seq_out = ~0ull;
    return &h;
  }

  uint64_t n = r->next.load(std::memory_order_relaxed);
  Entry& e = r->entries[n & r->mask];
  e.seq.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  e.t_begin = Now();
  e.t_end.store(0, std::memory_order_relaxed);
  e.site = &site;
  const PPCRegister* regs = &ctx.r3;
  for (int i = 0; i < 8; ++i) e.args[i] = regs[i].u32;
  e.lr = lr;
  e.result = 0;
  e.has_out = 0;
  DecodeBegin(*d, e);
  e.seq.store(n + 1, std::memory_order_release);
  r->next.store(n + 1, std::memory_order_release);
  if ((n & 4095) == 0 && n != r->name_refresh) {
    r->name_refresh = n;
    RefreshName(r);
  }
  tls_site = &site;
  tls_lr = lr;
  *seq_out = n + 1;
  return &e;
}

void End(Entry* entry, uint64_t seq, Site& site, PPCContext& ctx, uint8_t* /*base*/) {
  if (entry->seq.load(std::memory_order_relaxed) != seq) return;  // overwritten by nested calls
  if (site.returns) entry->result = ctx.r3.u32;
  if (seq != ~0ull) {
    const Decode* d = site.decode.load(std::memory_order_relaxed);
    if (d && d->out_arg >= 0 && (!site.returns || int32_t(ctx.r3.u32) >= 0)) {
      if (const uint8_t* p = Guest<uint8_t>(entry->args[d->out_arg])) {
        entry->out = Be32(p);
        entry->has_out = 1;
      }
    }
    tls_site = nullptr;
  }
  entry->t_end.store(Now(), std::memory_order_release);
}

void NoteHandle(char op, uint32_t handle, uint32_t object_type, int32_t ref_count) {
  if (!g_handle_ring) return;
  uint64_t n = g_handle_next.fetch_add(1, std::memory_order_relaxed);
  HandleEv& h = g_handle_ring[n & g_handle_mask];
  h.seq.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  h.t = Now();
  h.site = tls_site;
  h.lr = tls_site ? tls_lr : 0;
  h.ring = tls_ring;
  h.host_tid = HostTid();
  h.handle = handle;
  h.ref_count = ref_count;
  h.op = uint8_t(op);
  h.type = uint8_t(object_type);
  h.seq.store(n + 1, std::memory_order_release);
}

void NoteFatal(const char* reason) {
  if (!g_ring_entries || !reason) return;
  snprintf(g_fatal_buf, sizeof(g_fatal_buf), "%s", reason);
  g_fatal_note.store(g_fatal_buf, std::memory_order_release);
}

const char* DumpNow(const char* reason) {
  if (!g_ring_entries || !g_started.load(std::memory_order_acquire)) return nullptr;
  return DoDump(reason ? reason : "request", 0, false);
}

void DumpFromSignal(int sig) {
  if (!g_ring_entries || !g_started.load(std::memory_order_acquire)) return;
  if (g_crash_dumped.exchange(true)) return;  // one crash dump per process
  DoDump(SignalReason(sig), sig, true);
}

void DumpFatal(const char* reason) {
  if (!g_ring_entries || !g_started.load(std::memory_order_acquire)) return;
  if (g_crash_dumped.exchange(true)) return;
  DoDump(reason, 0, true);
}

}  // namespace rex::kcall
