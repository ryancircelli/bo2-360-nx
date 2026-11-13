// The game's streaming memory allocator search, natively (bo2zm_native_stream_alloc), with a checked phase.
//
// What the game does
// ------------------
// sub_828F1C10(size r3, priority f1, out r5) finds room for a block in the streaming pool. Two tables, both sorted by
// address, describe the pool: A, the blocks in use (20 bytes each at 0x84BA9F00: start +0, end +4, a skip flag byte
// +16; count at 0x84BCF800), and B, the free blocks ({start, end}, 8 bytes each at 0x84BC3E00; count at 0x84BCF804).
// An iterator (sub_828EF578 initializes it, sub_828EF698 steps it; 24 bytes: state, current address, cursor into A,
// cursor into B, A's and B's first starts) walks both in address order: state 1 = at a free block, 2 = at a block in
// use, 3 = between runs, 0 = done. The search:
//   1. first fit: the first free block whose 4 KB-aligned start leaves room -> sub_828EF108(start, start + size),
//      *out = start, return 5;
//   2. otherwise, for every free block, the window that extends it over the next adjacent blocks (free ones add their
//      size, used ones are candidates to evict) until it is large enough; the best window is the one whose largest
//      evicted block is smallest, then with the fewest blocks of that size -> return sub_828F1A88(window start,
//      window end, size, priority, out);
//   3. no window -> sub_828F16F8(size, priority, out), which evicts by priority; a 2 from it starts over at the top.
// Before the scan it may call sub_828F0F98 or sub_828F1170 (pool states at 0x84BCF828/2C/30), and it returns 1
// ("later") when sub_828EF958 says so.
//
// Why natively
// ------------
// In a Town match the search ran 6,000-130,000 times per 10 s on the PC, nearly all of them failing (the pool is full
// and the same requests come back), each walking ~1,300-1,450 iterator steps over ~1,130 blocks in use: 17-24 us per
// search, up to 2.75 s of CPU per 10 s (measured with the "[stream_alloc]" lines described below). The iterator and
// the scan only read the two tables, so they are done here with plain loads and the iterator inlined; every call the
// game's function makes (the pool-state calls, sub_828EF958, sub_828EF108, sub_828F1A88, sub_828F16F8) is made the
// same way, with the same arguments and return addresses. Nothing else changes. And since ~99 % of the searches
// repeat a (size, tables) pair the thread saw among its last 64 (the pool is full: the same requests, and the block
// mover's free/search/restore, come back unchanged), each scan's outcome is remembered with a 128-bit hash of the
// tables (ScanRemembered). PC: 17-24 us per search with the game's code, 8-9 with the native scan, ~3 with the
// remembered outcomes; all searches hold critical section 6 (counted).
//
// The guards
// ----------
// For the first kChecked searches, then 1 in kCheckEvery, the game's own function runs instead, and each time it
// starts a scan (its first iterator initialization, sub_828EF578 from 0x828F1CA8) the native scan predicts the
// outcome from the same tables; the outcome call the game's function then makes (sub_828EF108, sub_828F1A88 or
// sub_828F16F8, with its arguments) must match. A difference logs a MISMATCH and turns the native search off for the
// rest of the run. A remembered outcome is checked the same way (the first kChecked reuses, then 1 in kCheckEvery, scan
// anyway and compare); a difference turns the reuse off. Every 10 s "[stream_alloc]" lines log the searches, time,
// iterator steps, results and checks.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(bo2zm_native_stream_alloc, true, "BO2",
                    "The streaming pool's allocator search (sub_828F1C10) in native code, checked against the game's "
                    "own first (bo2zm_stream_alloc.cpp); false = the game's code only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_828F1C10);
REX_EXTERN(__imp__sub_828EF698);
REX_EXTERN(__imp__sub_828EF578);
REX_EXTERN(__imp__sub_828EF108);
REX_EXTERN(__imp__sub_828F1A88);
REX_EXTERN(__imp__sub_828F16F8);
REX_EXTERN(__imp__sub_828EF958);
REX_EXTERN(__imp__sub_828F0F98);
REX_EXTERN(__imp__sub_828F1170);

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kPool = 0x84BA9F00;            // table A's first entry; the pool's globals are relative to it
constexpr uint32_t kCountA = kPool + 0x25900;     // 0x84BCF800
constexpr uint32_t kCountB = kPool + 0x25904;     // 0x84BCF804
constexpr uint32_t kTableB = kPool + 0x19F00;     // 0x84BC3E00
constexpr uint32_t kStateA = kPool + 0x25B28;     // pool states read before the scan
constexpr uint32_t kStateB = kPool + 0x25B2C;
constexpr uint32_t kStateC = kPool + 0x25B30;
constexpr uint32_t kEntryA = 20;
constexpr uint32_t kEntryB = 8;
constexpr uint32_t kFrame = 0x110;                // sub_828F1C10's stack frame (stwu r1,-0x110(r1))
constexpr uint32_t kReturnFromJob = 0x828F2440;   // sub_828F2398's call
constexpr uint32_t kInitScan = 0x828F1CA8;        // return address of the scan's first iterator initialization

// Return addresses of the calls sub_828F1C10 makes (ctx.lr as its own code sets them).
constexpr uint32_t kLrState1170 = 0x828F1C74;
constexpr uint32_t kLrState0F98 = 0x828F1C84;
constexpr uint32_t kLrLater1 = 0x828F1C88;
constexpr uint32_t kLrFallback = 0x828F1E8C;
constexpr uint32_t kLrLater2 = 0x828F1E98;
constexpr uint32_t kLrWindow = 0x828F1EC8;
constexpr uint32_t kLrPlace = 0x828F1EE0;

// The game's critical section 6 (its table at 0x83DE0218, 28 bytes each): owning thread at +0x18, the KTHREAD the
// SDK stores (XThread guest object), which the thread's r13 block holds at +0x100.
constexpr uint32_t kLock6Owner = 0x83DE0218 + 6 * 28 + 0x18;

constexpr uint64_t kChecked = 20000;
constexpr uint64_t kCheckEvery = 1024;

bool g_on = false;
std::atomic<bool> g_off{false};  // a MISMATCH
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_checks{0};
std::atomic<uint64_t> g_mismatches{0};

// Measurement.
std::atomic<uint64_t> g_searches{0};
std::atomic<uint64_t> g_searches_job{0};
std::atomic<uint64_t> g_searches_unlocked{0};  // searches made without critical section 6
std::atomic<uint64_t> g_native{0};
std::atomic<uint64_t> g_ns{0};
std::atomic<uint64_t> g_steps{0};
std::atomic<uint64_t> g_ns_scan{0};      // native searches: in the scan itself
std::atomic<uint64_t> g_ns_fallback{0};  // native searches: in sub_828F16F8
std::atomic<uint64_t> g_ns_states{0};    // native searches: in the pool-state calls
std::atomic<uint64_t> g_ns_later{0};     // native searches: in sub_828EF958 (the "later" check)
// The scans' outcomes remembered (ScanRemembered).
std::atomic<uint64_t> g_remembered{0};      // scans not made: the same size and tables were scanned recently
std::atomic<uint64_t> g_remember_checks{0};
std::atomic<bool> g_remember_off{false};
std::atomic<uint64_t> g_states_0f98{0};
std::atomic<uint64_t> g_states_1170{0};
std::array<std::atomic<uint64_t>, 8> g_results{};
std::atomic<int64_t> g_last_report_ns{0};

inline uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
inline uint8_t Read8(const uint8_t* base, uint32_t address) { return base[address]; }
inline void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

// --- The iterator (sub_828EF578, sub_828EF698), transcribed -------------------------------------------------------
struct It {
  uint32_t state = 0;
  uint32_t cur = 0;
  uint32_t a = 0;       // cursor into A (guest address of an entry), 0 = past the end
  uint32_t b = 0;       // cursor into B
  uint32_t head_a = 0;  // A's first start (written by the initialization only)
  uint32_t head_b = 0;
};

void Init(const uint8_t* base, It& it) {
  it = It{};
  it.state = 3;
  const int32_t count_a = int32_t(Read32(base, kCountA));
  if (count_a > 0) {
    it.a = kPool;
    it.head_a = Read32(base, kPool);
    it.cur = Read32(base, kPool);
    if (Read8(base, kPool + 16) != 0) {
      it.a = kPool + kEntryA;
      if (it.a == kPool + uint32_t(count_a) * kEntryA) {
        it.a = 0;
      }
    } else {
      it.state = 2;
    }
  }
  if (int32_t(Read32(base, kCountB)) > 0) {
    it.b = kTableB;
    it.head_b = Read32(base, kTableB);
    const uint32_t start = Read32(base, kTableB);
    if (start < it.cur || it.a == 0) {
      it.cur = start;
      it.state = 1;
    }
  }
}

inline uint32_t EndA(const uint8_t* base) { return kPool + Read32(base, kCountA) * kEntryA; }
inline uint32_t EndB(const uint8_t* base) { return kTableB + Read32(base, kCountB) * kEntryB; }

// The B cursor's next free block starts where 'end' is: state 1; otherwise 3 (sub_828EF698 at 0x828EF6FC).
inline uint32_t AfterRun(const uint8_t* base, It& it, uint32_t end) {
  if (it.b != 0 && Read32(base, it.b) == end) {
    it.state = 1;
    return 1;
  }
  it.state = 3;
  return 3;
}

// A flagged block in use is skipped: the A cursor moves past it (0 at the end) and the state is 3.
inline uint32_t SkipFlagged(const uint8_t* base, It& it, uint32_t entry) {
  it.a = entry + kEntryA;
  if (it.a == EndA(base)) {
    it.a = 0;
  }
  it.state = 3;
  return 3;
}

uint32_t Step(const uint8_t* base, It& it, uint32_t mode) {
  if (mode == 0 || mode > 3) {
    return 0;  // no state is stored
  }
  if (mode == 2) {  // from a block in use
    const uint32_t entry = it.a;
    const uint32_t next = entry + kEntryA;
    const uint32_t end = Read32(base, entry + 4);
    it.cur = end;
    if (next == EndA(base)) {
      it.a = 0;
      return AfterRun(base, it, end);
    }
    it.a = next;
    if (Read32(base, next) != end) {
      return AfterRun(base, it, end);
    }
    if (Read8(base, next + 16) == 0) {
      it.state = 2;
      return 2;
    }
    return SkipFlagged(base, it, next);
  }
  if (mode == 1) {  // from a free block
    const uint32_t entry = it.b;
    const uint32_t next = entry + kEntryB;
    const uint32_t end = Read32(base, entry + 4);
    it.cur = end;
    if (next == EndB(base)) {
      it.b = 0;
      if (it.a == 0 || Read32(base, it.a) != end) {
        it.state = 3;
        return 3;
      }
      if (Read8(base, it.a + 16) == 0) {
        it.state = 2;
        return 2;
      }
      return SkipFlagged(base, it, it.a);
    }
    it.b = next;
    if (it.a == 0) {
      return AfterRun(base, it, end);
    }
    if (Read32(base, it.a) != end) {
      return AfterRun(base, it, end);
    }
    if (Read8(base, it.a + 16) == 0) {
      it.state = 2;
      return 2;
    }
    return SkipFlagged(base, it, it.a);
  }
  // mode 3: the lower of the two heads
  if (it.b != 0 && it.a != 0) {
    if (Read32(base, it.b) >= Read32(base, it.a)) {
      it.cur = Read32(base, it.a);
      it.state = 2;
      return 2;
    }
    it.cur = Read32(base, it.b);
    it.state = 1;
    return 1;
  }
  if (it.a != 0) {
    it.cur = Read32(base, it.a);
    it.state = 2;
    return 2;
  }
  if (it.b != 0) {
    it.cur = Read32(base, it.b);
    it.state = 1;
    return 1;
  }
  it.state = 0;
  return 0;
}

// --- The scan (sub_828F1C10 from 0x828F1C9C to the outcome), transcribed --------------------------------------------
struct Outcome {
  enum Kind : uint8_t { kPlace, kWindow, kFallback } kind = kFallback;
  uint32_t a = 0;  // kPlace: the start; kWindow: the window's start
  uint32_t b = 0;  // kWindow: the window's end
};

Outcome Scan(const uint8_t* base, uint32_t size_raw, uint64_t& steps) {
  const int32_t size = int32_t(size_raw);
  It it1;
  It it2;
  Init(base, it1);
  Init(base, it2);  // the game initializes both (the second is overwritten before use)
  uint32_t r3 = it1.state;
  uint32_t r21 = 0, r22 = 0;
  int32_t r23 = 0x7FFFFFFF, r25 = 0x7FFFFFFF;
  for (;;) {
    // To the next free block (state 1), or the end.
    while (r3 != 1) {
      if (r3 == 0) {
        Outcome o;
        if (r22 != 0) {
          o.kind = Outcome::kWindow;
          o.a = r22;
          o.b = r21;
        }
        return o;
      }
      r3 = Step(base, it1, r3);
      ++steps;
    }
    const uint32_t block = it1.b;
    const uint32_t start = it1.cur;
    const uint32_t free_size = Read32(base, block + 4) - Read32(base, block);
    uint32_t r30 = free_size + start;
    const uint32_t r26 = (start + 0xFFF) & 0xFFFFF000u;
    uint32_t r29 = r30 - r26;
    if (int32_t(r29) >= size) {
      Outcome o;
      o.kind = Outcome::kPlace;
      o.a = r26;
      return o;
    }
    uint32_t r27 = 0;
    int32_t r31 = 0;
    int32_t r28 = 0;
    it2 = it1;
    uint32_t s = Step(base, it2, it2.state);
    ++steps;
    bool next = false;  // to the next free block from it1 (0x828F1E64)
    for (;;) {
      if (s == 0) {
        next = true;
        break;
      }
      if (s == 2) {
        const uint32_t entry = it2.a;
        const int32_t used = int32_t(Read32(base, entry + 4) - Read32(base, entry));
        if (used >= size) {
          next = true;
          break;
        }
        if (used > r31) {
          if (used > r25 || (r23 <= 1 && used == r25)) {
            next = true;
            break;
          }
          r31 = used;
          r28 = 1;
          r27 += uint32_t(used);
          r30 += uint32_t(used);
        } else {
          if (used == r31) {
            if (r28 >= r23 && r25 == r31) {
              next = true;
              break;
            }
            ++r28;
          }
          r27 += uint32_t(used);
          r30 += uint32_t(used);
        }
      } else if (s == 1) {
        const uint32_t entry = it2.b;
        const uint32_t more = Read32(base, entry + 4) - Read32(base, entry);
        r29 += more;
        r30 += more;
      } else if (s == 3) {
        next = true;
        break;
      }
      if (int32_t(r29 + r27) >= size) {
        // A window large enough: the best so far?
        if (r31 < r25 || (r31 == r25 && r28 < r23)) {
          r25 = r31;
          r23 = r28;
          r22 = r26;
          r21 = r30;
        }
        next = true;
        break;
      }
      s = Step(base, it2, it2.state);
      ++steps;
      if (s == 0) {
        // it2 ran out: it1 steps once and the outer loop goes on from its new state (0x828F1E30)
        r3 = Step(base, it1, it1.state);
        ++steps;
        break;
      }
    }
    if (next) {
      r3 = Step(base, it1, it1.state);
      ++steps;
    }
  }
}

// --- Remembered outcomes ---------------------------------------------------------------------------------------------
// The scan is a pure function of the size and of the bytes it reads: the two counts and the two tables (A's entries
// whole, 20 bytes each, and B's). In a match ~98-99 % of the searches repeat a (size, tables) pair among the thread's
// last 64 (measured on the PC): the pool is full and the same requests (and the block mover's free/search/restore)
// come back unchanged. So each scan's outcome is kept with a 128-bit XXH3 of those bytes; the same size and hash give
// the same outcome without walking the tables (hashing them costs ~0.8 us on the PC, the scan ~4.2). The calls that
// follow are made as always. Checked: the first kChecked reuses, then 1 in kCheckEvery, scan anyway and compare.
constexpr size_t kRemembered = 64;

struct Remembered {
  uint64_t low = 0;
  uint64_t high = 0;
  uint32_t size = 0;
  bool valid = false;
  Outcome outcome;
};
thread_local std::array<Remembered, kRemembered> t_remembered{};
thread_local uint32_t t_remembered_next = 0;
thread_local uint64_t t_remembered_sampled = 0;

bool Same(const Outcome& x, const Outcome& y) { return x.kind == y.kind && x.a == y.a && x.b == y.b; }

// Two one-shot hashes chained by their seeds. (The first version used the streaming API on a stack state without
// XXH3_INITSTATE: with a stale seed field the library may skip deriving its secret, and the hash then missed changes,
// which the guard caught as different outcomes for "equal" tables: run_20261019_102709, B differing at byte 225.)
XXH128_hash_t HashTables(const uint8_t* base, uint32_t count_a, uint32_t count_b) {
  const XXH128_hash_t a =
      XXH3_128bits_withSeed(base + kPool, size_t(count_a) * kEntryA, (uint64_t(count_a) << 32) | count_b);
  return XXH3_128bits_withSeed(base + kTableB, size_t(count_b) * kEntryB, a.low64 ^ a.high64 ^ 0x9E3779B97F4A7C15ull);
}

std::atomic<uint64_t> g_remember_raced{0};  // scans whose tables changed while they ran: not remembered
std::atomic<uint64_t> g_check_raced{0};     // checks whose own scan saw the tables change: inconclusive

Outcome ScanRemembered(const uint8_t* base, uint32_t size, uint64_t& steps) {
  if (g_remember_off.load(std::memory_order_relaxed)) {
    return Scan(base, size, steps);
  }
  const uint32_t count_a = Read32(base, kCountA);
  const uint32_t count_b = Read32(base, kCountB);
  // The counts are part of what the scan reads; bounded like the game's tables would be, or no reuse.
  if (count_a > 0x10000 || count_b > 0x10000) {
    return Scan(base, size, steps);
  }
  const XXH128_hash_t h = HashTables(base, count_a, count_b);
  for (const Remembered& r : t_remembered) {
    if (r.valid && r.size == size && r.low == h.low64 && r.high == h.high64) {
      const uint64_t n = g_remember_checks.load(std::memory_order_relaxed);
      if (n < kChecked || (++t_remembered_sampled % kCheckEvery) == 0) {
        g_remember_checks.fetch_add(1, std::memory_order_relaxed);
        const Outcome scanned = Scan(base, size, steps);
        const XXH128_hash_t after = HashTables(base, Read32(base, kCountA), Read32(base, kCountB));
        if (after.low64 != h.low64 || after.high64 != h.high64) {
          g_check_raced.fetch_add(1, std::memory_order_relaxed);  // the tables changed under this check's scan
          return scanned;
        }
        if (!Same(scanned, r.outcome)) {
          if (!g_remember_off.exchange(true, std::memory_order_relaxed)) {
            REXLOG_ERROR("[stream_alloc] remembered outcome MISMATCH after {} checks (size {:08X}): kind {} {:08X} "
                         "{:08X} remembered, the scan gives kind {} {:08X} {:08X}; reuse is OFF for the session",
                         n, size, int(r.outcome.kind), r.outcome.a, r.outcome.b, int(scanned.kind), scanned.a,
                         scanned.b);
          }
          return scanned;
        }
      }
      g_remembered.fetch_add(1, std::memory_order_relaxed);
      return r.outcome;
    }
  }
  const Outcome o = Scan(base, size, steps);
  // Other threads may change the tables while the scan runs (the first version remembered such a scan and a later
  // check caught it: run_20261019_101159). Only an outcome whose tables were the same before and after is kept.
  const XXH128_hash_t after = HashTables(base, Read32(base, kCountA), Read32(base, kCountB));
  if (after.low64 != h.low64 || after.high64 != h.high64) {
    g_remember_raced.fetch_add(1, std::memory_order_relaxed);
    return o;
  }
  Remembered& slot = t_remembered[t_remembered_next++ % kRemembered];
  slot.low = h.low64;
  slot.high = h.high64;
  slot.size = size;
  slot.outcome = o;
  slot.valid = true;
  return o;
}

// --- Calls into the game's code -------------------------------------------------------------------------------------
using GuestFunction = void (*)(PPCContext&, uint8_t*);

// A call as sub_828F1C10 makes it: its frame below the caller's stack pointer, lr = the return address.
inline void Call(PPCContext& ctx, uint8_t* base, GuestFunction f, uint32_t lr) {
  ctx.lr = lr;
  f(ctx, base);
}

// --- The checked phase: predictions and the outcome calls ------------------------------------------------------------
struct Check {
  bool active = false;     // this thread is running the game's function under check
  bool predicted = false;  // a prediction waits for its outcome call
  Outcome prediction;
  uint32_t size = 0;
  uint32_t out = 0;
};
thread_local Check t_check;

void Mismatch(const char* what, const Outcome& p, uint32_t a, uint32_t b) {
  g_mismatches.fetch_add(1, std::memory_order_relaxed);
  if (!g_off.exchange(true, std::memory_order_relaxed)) {
    REXLOG_ERROR("[stream_alloc] MISMATCH after {} checked searches: the game's search went to {} ({:08X}, {:08X}), "
                 "the native scan predicted {} ({:08X}, {:08X}); the native search is OFF for the session",
                 g_checks.load(std::memory_order_relaxed), what, a, b,
                 p.kind == Outcome::kPlace ? "place" : p.kind == Outcome::kWindow ? "window" : "fallback", p.a, p.b);
  }
}

// An outcome call of the game's function under check: it must be the predicted one.
void Verify(Outcome::Kind kind, const char* what, uint32_t a, uint32_t b) {
  if (!t_check.active || !t_check.predicted) {
    return;
  }
  t_check.predicted = false;
  const Outcome& p = t_check.prediction;
  bool same = p.kind == kind;
  if (same && kind == Outcome::kPlace) {
    same = p.a == a && p.a + t_check.size == b;
  } else if (same && kind == Outcome::kWindow) {
    same = p.a == a && p.b == b;
  }
  if (!same) {
    Mismatch(what, p, a, b);
  }
}

void Report(const uint8_t* base) {
  const int64_t now =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  int64_t last = g_last_report_ns.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10'000'000'000 || !g_last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  const double seconds = double(now - last) / 1e9;
  const uint64_t searches = g_searches.exchange(0, std::memory_order_relaxed);
  const uint64_t from_job = g_searches_job.exchange(0, std::memory_order_relaxed);
  const uint64_t unlocked = g_searches_unlocked.exchange(0, std::memory_order_relaxed);
  const uint64_t native = g_native.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t steps = g_steps.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_scan = g_ns_scan.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_fallback = g_ns_fallback.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_states = g_ns_states.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_later = g_ns_later.exchange(0, std::memory_order_relaxed);
  const uint64_t remembered = g_remembered.exchange(0, std::memory_order_relaxed);
  const uint64_t states_0f98 = g_states_0f98.exchange(0, std::memory_order_relaxed);
  const uint64_t states_1170 = g_states_1170.exchange(0, std::memory_order_relaxed);
  uint64_t results[8];
  for (int i = 0; i < 8; ++i) {
    results[i] = g_results[i].exchange(0, std::memory_order_relaxed);
  }
  REXLOG_INFO("[stream_alloc] {} of {} searches ran without critical section 6 held by their thread", unlocked,
              searches);
  REXLOG_INFO("[stream_alloc] remembered outcomes{}: {} of {} native searches did not scan (the same size and tables "
              "among the thread's last {}); {} checked by scanning anyway, 0 differences so far; {} scans not kept (the tables "
              "changed while they ran); {} checks inconclusive (the tables changed under their scan)",
              g_remember_off.load(std::memory_order_relaxed) ? " (OFF after a MISMATCH)" : "", remembered, native,
              kRemembered, g_remember_checks.load(std::memory_order_relaxed),
              g_remember_raced.load(std::memory_order_relaxed), g_check_raced.load(std::memory_order_relaxed));
  REXLOG_INFO("[stream_alloc] {:.0f} s: {} searches ({} from the lock 6 job, {} native), {:.1f} ms in them ({:.1f} us "
              "each; native ones: {:.1f} us scanning, {:.1f} us in the eviction fallback, {:.1f} us in the pool-state "
              "calls ({} sub_828F0F98, {} sub_828F1170), {:.1f} us in sub_828EF958), {:.0f} native iterator "
              "steps per native search; results: 1 (later) {}, 2 {}, 3 (no room) {}, "
              "5 (placed) {}, other {}; tables now A {} B {}; checks {} ({} differences){}",
              seconds, searches, from_job, native, double(ns) / 1e6,
              searches ? double(ns) / 1e3 / double(searches) : 0.0,
              native ? double(ns_scan) / 1e3 / double(native) : 0.0,
              native ? double(ns_fallback) / 1e3 / double(native) : 0.0,
              native ? double(ns_states) / 1e3 / double(native) : 0.0, states_0f98, states_1170,
              native ? double(ns_later) / 1e3 / double(native) : 0.0,
              native ? double(steps) / double(native) : 0.0,
              results[1], results[2], results[3], results[5], results[0] + results[4] + results[6] + results[7],
              Read32(base, kCountA), Read32(base, kCountB), g_checks.load(std::memory_order_relaxed),
              g_mismatches.load(std::memory_order_relaxed),
              g_off.load(std::memory_order_relaxed) ? " [native OFF after a MISMATCH]"
                                                    : (g_on ? "" : " [native off by bo2zm_native_stream_alloc]"));
  // The pool's extent and how full it is, from the two tables (read without the lock: a snapshot that may be off by a
  // block while another thread changes them).
  const uint32_t count_a = std::min<uint32_t>(Read32(base, kCountA), 8192);
  const uint32_t count_b = std::min<uint32_t>(Read32(base, kCountB), 8192);
  uint32_t lo = UINT32_MAX, hi = 0;
  uint64_t used = 0, free = 0;
  for (uint32_t i = 0; i < count_a; ++i) {
    const uint32_t start = Read32(base, kPool + i * 20), end = Read32(base, kPool + i * 20 + 4);
    if (end > start) {
      lo = std::min(lo, start);
      hi = std::max(hi, end);
      used += end - start;
    }
  }
  for (uint32_t i = 0; i < count_b; ++i) {
    const uint32_t start = Read32(base, kTableB + i * 8), end = Read32(base, kTableB + i * 8 + 4);
    if (end > start) {
      lo = std::min(lo, start);
      hi = std::max(hi, end);
      free += end - start;
    }
  }
  REXLOG_INFO("[stream_alloc] pool {:08X}-{:08X} ({:.1f} MB): {} blocks in use ({:.1f} MB), {} free ({:.1f} MB)", lo,
              hi, hi > lo ? double(hi - lo) / 1048576.0 : 0.0, count_a, double(used) / 1048576.0, count_b,
              double(free) / 1048576.0);
}

// The whole of sub_828F1C10, natively.
void Search(PPCContext& ctx, uint8_t* base) {
  const uint32_t lr_entry = uint32_t(ctx.lr);
  const uint32_t r1_entry = ctx.r1.u32;
  const uint32_t size = ctx.r3.u32;
  const uint32_t out = ctx.r5.u32;
  const double priority = ctx.f1.f64;
  ctx.r1.u32 = r1_entry - kFrame;
  Write32(base, ctx.r1.u32, r1_entry);  // stwu's back chain
  uint32_t result = 0;
  // The pool states (0x828F1C20-0x828F1C80), with the registers the game's code leaves for those calls.
  const uint32_t state_a = Read32(base, kStateA);
  const auto t_states = Clock::now();
  if (state_a != 0 || Read32(base, kStateB) != 0) {
    g_states_0f98.fetch_add(1, std::memory_order_relaxed);
    // cntlzw r11,r10 / rlwinm r3,r11,27,31,31: r3 = 1 when the first state is 0 (the second one is set).
    ctx.r10.u64 = state_a;
    ctx.r11.u64 = state_a == 0 ? 32 : uint64_t(__builtin_clz(state_a));
    ctx.r3.u64 = state_a == 0 ? 1 : 0;
    Call(ctx, base, __imp__sub_828F0F98, kLrState0F98);
  } else if (Read32(base, kStateC) != 0) {
    g_states_1170.fetch_add(1, std::memory_order_relaxed);
    Call(ctx, base, __imp__sub_828F1170, kLrState1170);
  }
  g_ns_states.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_states).count()),
      std::memory_order_relaxed);
  const auto t_later = Clock::now();
  Call(ctx, base, __imp__sub_828EF958, kLrLater1);
  g_ns_later.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_later).count()),
      std::memory_order_relaxed);
  if ((ctx.r3.u32 & 0xFF) != 0) {
    result = 1;
  } else {
    for (;;) {
      uint64_t steps = 0;
      const auto t_scan = Clock::now();
      const Outcome o = ScanRemembered(base, size, steps);
      g_ns_scan.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_scan).count()),
          std::memory_order_relaxed);
      g_steps.fetch_add(steps, std::memory_order_relaxed);
      if (o.kind == Outcome::kPlace) {
        ctx.r3.u64 = o.a;
        ctx.r4.u64 = o.a + size;
        Call(ctx, base, __imp__sub_828EF108, kLrPlace);
        Write32(base, out, o.a);
        result = 5;
        break;
      }
      if (o.kind == Outcome::kWindow) {
        ctx.r3.u64 = o.a;
        ctx.r4.u64 = o.b;
        ctx.r5.u64 = size;
        ctx.r7.u64 = out;
        ctx.f1.f64 = priority;
        Call(ctx, base, __imp__sub_828F1A88, kLrWindow);
        result = ctx.r3.u32;
        break;
      }
      ctx.r3.u64 = size;
      ctx.r5.u64 = out;
      ctx.f1.f64 = priority;
      const auto t_fallback = Clock::now();
      Call(ctx, base, __imp__sub_828F16F8, kLrFallback);
      g_ns_fallback.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_fallback).count()),
          std::memory_order_relaxed);
      if (ctx.r3.u32 != 2) {
        result = ctx.r3.u32;
        break;
      }
      Call(ctx, base, __imp__sub_828EF958, kLrLater2);
      if ((ctx.r3.u32 & 0xFF) != 0) {
        result = 1;
        break;
      }
    }
  }
  ctx.r3.u64 = result;
  ctx.r1.u32 = r1_entry;
  ctx.lr = lr_entry;
}

}  // namespace

// The search: native, or the game's own under check.
REX_HOOK_RAW(sub_828F1C10) {
  static const bool on = [] {
    g_on = REXCVAR_GET(bo2zm_native_stream_alloc);
    return g_on;
  }();
  const uint32_t lr = uint32_t(ctx.lr);
  const auto t0 = Clock::now();
  const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed);
  if (Read32(base, kLock6Owner) != Read32(base, ctx.r13.u32 + 0x100)) {
    g_searches_unlocked.fetch_add(1, std::memory_order_relaxed);
  }
  if (on && !g_off.load(std::memory_order_relaxed) && n >= kChecked && n % kCheckEvery != 0) {
    Search(ctx, base);
    g_native.fetch_add(1, std::memory_order_relaxed);
  } else {
    const bool check = on && !g_off.load(std::memory_order_relaxed);
    if (check) {
      t_check.active = true;
      t_check.predicted = false;
      t_check.size = ctx.r3.u32;
      t_check.out = ctx.r5.u32;
      g_checks.fetch_add(1, std::memory_order_relaxed);
    }
    __imp__sub_828F1C10(ctx, base);
    t_check.active = false;
  }
  g_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()),
                 std::memory_order_relaxed);
  g_searches.fetch_add(1, std::memory_order_relaxed);
  if (lr == kReturnFromJob) {
    g_searches_job.fetch_add(1, std::memory_order_relaxed);
  }
  g_results[ctx.r3.u32 & 7].fetch_add(1, std::memory_order_relaxed);
  Report(base);
}

// The game's function under check starts a scan: predict its outcome from the same tables.
REX_HOOK_RAW(sub_828EF578) {
  if (t_check.active && uint32_t(ctx.lr) == kInitScan) {
    uint64_t steps = 0;
    t_check.prediction = Scan(base, t_check.size, steps);
    t_check.predicted = true;
  }
  __imp__sub_828EF578(ctx, base);
}

// The three outcomes.
REX_HOOK_RAW(sub_828EF108) {
  if (t_check.active && uint32_t(ctx.lr) == kLrPlace) {
    Verify(Outcome::kPlace, "place", ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_828EF108(ctx, base);
}
REX_HOOK_RAW(sub_828F1A88) {
  if (t_check.active && uint32_t(ctx.lr) == kLrWindow) {
    Verify(Outcome::kWindow, "window", ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_828F1A88(ctx, base);
}
REX_HOOK_RAW(sub_828F16F8) {
  if (t_check.active && uint32_t(ctx.lr) == kLrFallback) {
    Verify(Outcome::kFallback, "fallback", ctx.r3.u32, 0);
  }
  __imp__sub_828F16F8(ctx, base);
}
