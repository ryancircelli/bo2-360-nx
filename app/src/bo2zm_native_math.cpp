// Hand-written replacements for five small recompiled math functions (bo2zm_native_math_*), each behind its own
// switch (off by default) and each checked against the generated code before it runs alone.
//
//   sub_8289B6F0  bo2zm_native_math_plane_test   point against a list of planes (20 bytes each): 1 on the first plane
//                                                whose distance is not above the float at 0x82000DC4, else 0
//   sub_8248B820  bo2zm_native_math_quat_matrix  quaternion (r3) to a 3x3 float matrix (r4)
//   sub_829170B0  bo2zm_native_math_sin          sin(f1), double polynomial
//   sub_82917190  bo2zm_native_math_cos          cos(f1), double polynomial
//   sub_829160B8  bo2zm_native_math_floor        floor(f1)
// The pure functions are in bo2zm_native_math.h (tools/tests/native_math_test.cpp compiles them against the generated
// bodies, one million random calls each). This file: the hooks, the switches and the checked mode.
//
// The checked mode. A checked call saves the context and the guest memory the function can write (sin: the 32 bytes
// below r1, cos: 16, quat_matrix: the 36 bytes at r4, the others none), runs the game's function, keeps what it
// produced, restores the input, runs the native one, and compares the written bytes plus r3, f1 and the FPSCR word
// (the registers callers read: a scan of every call site found no other register read after a call, see RESULT.md).
// The game's results are then put back: a checked call leaves exactly the game's state. A difference is tried once
// more (a concurrent writer of the inputs could cause one), then logs a MISMATCH with the inputs and turns that
// function off for the session. The first kChecked calls are all checked, then one in kCheckEvery per thread.
// bo2zm_native_math_check_only keeps every enabled function in checked mode forever (the game's results always win).
// Every ~10 s each enabled function logs its calls, calls/s, checked calls and mismatches.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "bo2zm_native_math.h"

REXCVAR_DEFINE_BOOL(bo2zm_native_math_plane_test, false, "BO2",
                    "Native sub_8289B6F0 (point against planes), checked against the game's code first "
                    "(bo2zm_native_math.cpp). false = the game's code only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_math_quat_matrix, false, "BO2",
                    "Native sub_8248B820 (quaternion to matrix), checked against the game's code first. false = the "
                    "game's code only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_math_sin, false, "BO2",
                    "Native sub_829170B0 (sin), checked against the game's code first. false = the game's code only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_math_cos, false, "BO2",
                    "Native sub_82917190 (cos), checked against the game's code first. false = the game's code only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_math_floor, false, "BO2",
                    "Native sub_829160B8 (floor), checked against the game's code first. false = the game's code only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_math_check_only, false, "BO2",
                    "Enabled bo2zm_native_math_* functions stay in checked mode forever (the game's result is always "
                    "the one used)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8289B6F0);
REX_EXTERN(__imp__sub_8248B820);
REX_EXTERN(__imp__sub_829170B0);
REX_EXTERN(__imp__sub_82917190);
REX_EXTERN(__imp__sub_829160B8);

namespace {

using namespace bo2zm::nmath;
using Clock = std::chrono::steady_clock;

constexpr uint64_t kChecked = 2000;
constexpr uint64_t kCheckEvery = 1024;
constexpr uint32_t kFlushEvery = 256;
constexpr uint32_t kMaxRegion = 64;

enum Phase : int { kChecking = 0, kNativeAlone = 1, kOff = 2 };

struct Stats {
  const char* name;
  std::atomic<int> phase{kChecking};
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> checked{0};
  std::atomic<uint64_t> mismatches{0};
  uint64_t last_calls = 0;  // under the report CAS
};
Stats g_stats[5] = {{"plane_test"}, {"quat_matrix"}, {"sin"}, {"cos"}, {"floor"}};
enum Fn { kPlane = 0, kQuat, kSin, kCos, kFloor };
std::atomic<int64_t> g_last_report{0};
std::atomic<uint64_t> g_plane_total{0};  // planes tested by sub_8289B6F0 (flushed with the calls)

struct Local {
  uint32_t pending[5] = {};
  uint32_t since_check[5] = {};
  uint64_t planes = 0;
};
thread_local Local t_local;

void Report() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  int64_t last = g_last_report.load(std::memory_order_relaxed);
  if (last != 0 && now - last < 10000) {
    return;
  }
  if (!g_last_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  if (last == 0) {
    return;  // the first call only starts the clock
  }
  const double seconds = double(now - last) / 1000.0;
  for (int i = 0; i < 5; ++i) {
    Stats& s = g_stats[i];
    const uint64_t calls = s.calls.load(std::memory_order_relaxed);
    if (calls == s.last_calls && calls == 0) {
      continue;
    }
    const int phase = s.phase.load(std::memory_order_relaxed);
    if (i == kPlane) {
      REXLOG_INFO("[native_math] {}: {} calls ({:.0f}/s), {:.2f} planes per call, {} checked, {} mismatches, {}", s.name,
                  calls, double(calls - s.last_calls) / seconds,
                  calls ? double(g_plane_total.load(std::memory_order_relaxed)) / double(calls) : 0.0,
                  s.checked.load(std::memory_order_relaxed), s.mismatches.load(std::memory_order_relaxed),
                  phase == kChecking ? "checking" : phase == kNativeAlone ? "native" : "OFF");
    } else {
      REXLOG_INFO("[native_math] {}: {} calls ({:.0f}/s), {} checked, {} mismatches, {}", s.name, calls,
                  double(calls - s.last_calls) / seconds, s.checked.load(std::memory_order_relaxed),
                  s.mismatches.load(std::memory_order_relaxed),
                  phase == kChecking ? "checking" : phase == kNativeAlone ? "native" : "OFF");
    }
    s.last_calls = calls;
  }
}

// Counts a call (flushed to the shared counters every kFlushEvery); true when this one must be checked.
inline bool CountAndDecide(Fn fn, bool check_only, int phase) {
  Local& l = t_local;
  if (++l.pending[fn] >= kFlushEvery) {
    g_stats[fn].calls.fetch_add(l.pending[fn], std::memory_order_relaxed);
    l.pending[fn] = 0;
    if (fn == kPlane && l.planes) {
      g_plane_total.fetch_add(l.planes, std::memory_order_relaxed);
      l.planes = 0;
    }
    Report();
  }
  if (check_only || phase == kChecking) {
    return true;
  }
  return ++l.since_check[fn] >= kCheckEvery ? (l.since_check[fn] = 0, true) : false;
}

const Tables& CachedTables(const uint8_t* base) {
  static const Tables t = LoadTables(base);
  return t;
}

void RunNative(Fn fn, PPCContext& ctx, uint8_t* base, const Tables& t) {
  switch (fn) {
    case kPlane: Sub8289B6F0(ctx, base); break;
    case kQuat: Sub8248B820(ctx, base); break;
    case kSin: Sub829170B0(ctx, base, t); break;
    case kCos: Sub82917190(ctx, base, t); break;
    case kFloor: Sub829160B8(ctx, t); break;
  }
}

// The guest memory the function can write, as (address, size).
void WrittenRegion(Fn fn, const PPCContext& ctx, uint32_t& at, uint32_t& size) {
  switch (fn) {
    case kQuat: at = ctx.r4.u32; size = 36; break;
    case kSin: at = ctx.r1.u32 - 32; size = 32; break;
    case kCos: at = ctx.r1.u32 - 16; size = 16; break;
    default: at = 0; size = 0; break;
  }
}

void Checked(Fn fn, void (*original)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base) {
  Stats& s = g_stats[fn];
  const PPCContext input = ctx;
  uint32_t at = 0, size = 0;
  WrittenRegion(fn, input, at, size);
  uint8_t before[kMaxRegion], after_original[kMaxRegion], after_native[kMaxRegion];
  if (size) {
    std::memcpy(before, base + at, size);
  }
  const Tables live = LoadTables(base);  // the tables are read live in a check
  const char* what = nullptr;
  uint32_t bad_byte = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    ctx = input;
    ctx.fpscr.setcsr(ctx.fpscr.csr);
    if (size) {
      std::memcpy(base + at, before, size);
    }
    original(ctx, base);
    const PPCContext result = ctx;
    if (size) {
      std::memcpy(after_original, base + at, size);
      std::memcpy(base + at, before, size);
    }
    ctx = input;
    ctx.fpscr.setcsr(ctx.fpscr.csr);
    RunNative(fn, ctx, base, live);
    if (size) {
      std::memcpy(after_native, base + at, size);
    }
    what = nullptr;
    if (ctx.r3.u64 != result.r3.u64) {
      what = "r3";
    } else if (ctx.f1.u64 != result.f1.u64) {
      what = "f1";
    } else if (ctx.fpscr.csr != result.fpscr.csr) {
      what = "fpscr";
    } else if (size && std::memcmp(after_original, after_native, size) != 0) {
      what = "memory";
      while (after_original[bad_byte] == after_native[bad_byte]) {
        ++bad_byte;
      }
    }
    // The game's result stands: its registers and its memory.
    const PPCContext native_result = ctx;
    ctx = result;
    if (size) {
      std::memcpy(base + at, after_original, size);
    }
    if (!what) {
      break;
    }
    if (attempt == 1) {
      s.phase.store(kOff, std::memory_order_relaxed);
      const uint64_t n = s.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
      REXLOG_ERROR("[native_math] MISMATCH in {} ({}) after {} checked calls (#{}): game r3 {:X} f1 {:016X} / native r3 "
                   "{:X} f1 {:016X}; byte {} of {} at {:08X}; inputs r1 {:08X} r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} "
                   "f1 {:016X}. This function is off for the session",
                   s.name, what, s.checked.load(std::memory_order_relaxed), n, result.r3.u64, result.f1.u64,
                   native_result.r3.u64, native_result.f1.u64, bad_byte, size, at, input.r1.u32, input.r3.u32,
                   input.r4.u32, input.r5.u32, input.r6.u32, input.f1.u64);
      return;
    }
  }
  const uint64_t n = s.checked.fetch_add(1, std::memory_order_relaxed) + 1;
  int expected = kChecking;
  if (n >= kChecked && !REXCVAR_GET(bo2zm_native_math_check_only) &&
      s.phase.compare_exchange_strong(expected, kNativeAlone)) {
    REXLOG_INFO("[native_math] {}: {} checked calls identical, the native version runs alone now (1 in {} still "
                "checked)",
                s.name, n, kCheckEvery);
  }
}

inline void Dispatch(Fn fn, bool on, void (*original)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base) {
  if (!on) {
    original(ctx, base);
    return;
  }
  const int phase = g_stats[fn].phase.load(std::memory_order_relaxed);
  if (phase == kOff) {
    original(ctx, base);
    return;
  }
  static const bool check_only = REXCVAR_GET(bo2zm_native_math_check_only);
  if (CountAndDecide(fn, check_only, phase)) {
    Checked(fn, original, ctx, base);
    return;
  }
  RunNative(fn, ctx, base, CachedTables(base));
}

}  // namespace

REX_HOOK_RAW(sub_8289B6F0) {
  static const bool on = REXCVAR_GET(bo2zm_native_math_plane_test);
  if (on && ctx.r6.s32 > 0) {
    t_local.planes += uint32_t(ctx.r6.s32);
  }
  Dispatch(kPlane, on, __imp__sub_8289B6F0, ctx, base);
}
REX_HOOK_RAW(sub_8248B820) {
  static const bool on = REXCVAR_GET(bo2zm_native_math_quat_matrix);
  Dispatch(kQuat, on, __imp__sub_8248B820, ctx, base);
}
REX_HOOK_RAW(sub_829170B0) {
  static const bool on = REXCVAR_GET(bo2zm_native_math_sin);
  Dispatch(kSin, on, __imp__sub_829170B0, ctx, base);
}
REX_HOOK_RAW(sub_82917190) {
  static const bool on = REXCVAR_GET(bo2zm_native_math_cos);
  Dispatch(kCos, on, __imp__sub_82917190, ctx, base);
}
REX_HOOK_RAW(sub_829160B8) {
  static const bool on = REXCVAR_GET(bo2zm_native_math_floor);
  Dispatch(kFloor, on, __imp__sub_829160B8, ctx, base);
}
