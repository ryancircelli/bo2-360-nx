// The game's entity loops (sub_822FD360 and the plane test sub_822FCF40 reaches), made cheaper without changing what
// they compute.
//
// What the game does
// ------------------
// sub_822FD360(world r3, first r4, end r5, class r6) is the body of four job functions that branch to it with r6 = 0
// (sub_822FD6D0), 1 (sub_822FD740), 2 (sub_822FD698) and 3 (sub_822FD708); each takes r3, r4, r5 from its job's data
// at +0, +16, +20, or, when +16 and +20 are both 0, r4 and r5 from [world + 880] + 24 and + 28 (the whole list).
// The list ([world + 880]): a ring of 1,024 u16 entity handles at +48 (slot i & 0x3FF); the entity is
// [world + 672] + handle * 16. List + 2096 counts the threads scanning it (sub_822E6B78 adds one, spinning while it
// is negative; the one that brings it back to 0 calls sub_822F4650, which, when [list + 2128] is set, takes the list
// from 0 to -1 and compacts the ring, freeing the entities with no reference left). For each i from r4 up to r5,
// skipping the entity whose handle is at list + 36 when [list + 40] > 0, it works out the entity's class:
//   def = [e + 160] (none: skipped); 3 when (u16 [def + 4] & 0x404) == 0x404, else 2 when u16 [def + 4] & 0x30,
//   else 1 when [e + 180] & 0xC00000 or u16 [e + 246] < 0x8000, else 0.
// For an entity of class r6: [e + 184] is swapped with the world's frame number [world + 720] (already that number:
// skipped), sub_822E6B20 adds one to the top 3 bits of [e + 252] (waiting until they are 0), a reference is taken on
// its low 14 bits when there is one (none: the top bits back down, skipped), then sub_822FCF40(world, e, e) and
// sub_822F4048(world, e, 0) (the reference given back), and the top bits back down (lwsync, atomic add 0xE0000000).
// Evidence: the PowerPC code of sub_822FD360 (from 0x822FD360), of its four callers (0x822FD698, 0x822FD6D0,
// 0x822FD708, 0x822FD740) and of its helpers sub_822E6B78, sub_822E6B20, sub_822F4048 and sub_822F4650.
//
// sub_822FCF40(world, e, e) does the per-entity work: sub_822F9BD8 (reads e + 246, e + 180, e + 48 .. e + 75 and
// from e + 132; writes e + 76 .. e + 103), the distance between e + 92..100 and e + 120..128 added to e + 196, the
// plane tests (sub_822FA0A0 calls sub_822FE3B8 for the world's two plane sets, at world + 480 and world + 160),
// sub_822FCCF8 and sub_822F2540, and finally e + 76..100 copied to e + 104..128.
//
// Where the time goes (a stack-sampled profile on an OLED at round 15 in Town on very low, self time summed over the
// busy part of the route): sub_822FD360 takes 4.6 % of a core summed over the threads (Main 1.1 % of its samples,
// each job worker about the same), sub_822FCF40 3.7 % (Main 1.5 %). About two thirds of sub_822FD360's samples are
// on the first loads of each entity it scans (ldr of e + 160 at 0x4b7c04, ldrh of e + 246 and of def + 4, ldr of
// e + 180): the entities are scattered through memory and cold, every class job scans the whole list, and the
// generated loop gets few of those misses going at once. In sub_822FCF40, a fifth of the samples are on its first loads
// of e + 124 and e + 96 (just after the sub_822F9BD8 call), and a third (0x481698-0x481784, half of it on Main) are
// sub_822FE3B8 inlined: per plane, the generated code writes FPCR twice (flush-to-zero on for the vector work, then off
// for the scalar compare) and reads back through guest memory the word it has just stored from a vector register.
//
// What this file does
// -------------------
// 1. bo2zm_native_entity_scan: a native pass over the ring slots the game's sub_822FD360 is about to scan reads each
//    entity's e + 160, e + 180, e + 246 and its def's + 4 without branching on them (so the core keeps many of those
//    misses in flight at once, and asks for the entity kScanAhead slots ahead), works out the class as above, and
//    asks for e + 48 .. e + 255 of the entities of r6's class (what sub_822FCF40 and sub_822F9BD8 read first). It
//    runs inside the game's function, right after its sub_822E6B78 call: from there until the function's end the
//    list cannot be compacted (sub_822F4650 takes it only from 0 readers to -1 and moves handles in the ring), so the
//    pass reads the ring, the entities and their defs exactly as protected as the game's loop does. It only reads guest
//    memory and leaves the registers as they were; the game's function goes on unchanged: only when memory arrives
//    changes, not what is computed. Every 10 s a "[native_entities]" line gives, for the calls with and without the
//    pass, the entities scanned and the time per call (the pass and the game's loop together).
// 2. bo2zm_native_plane_test: sub_822FE3B8 in native code. sub_822FE3B8(planes r3, count r4, box r5, box r6): both
//    boxes as unaligned 16-byte loads (lvlx/lvrx), and-ed with the vector at 0x83C288C0 (set at run time) minus the
//    one at 0x8205A2B0 ({0, 0, 0, 1.0f}), flush-to-zero on; for each of the count planes at r3 + 16 (aligned): the
//    corner (r6's where the plane's component is above the vector at 0x8205A2A0, zeros; else r5's), its 4-element
//    dot with the plane (vmsum4fp128) stored at r1 - 16 and read back as a float, and 1 returned when it is below the
//    float at 0x82000DC4 (0.0f); 0 after the last plane. The same simde operations as the generated code, with
//    flush-to-zero on for the whole loop and the compare done there too: the dot comes out of flush-to-zero
//    arithmetic (never a denormal) and the bound is read beforehand without it (a denormal bound: the game's code
//    runs), so the compare gives what the game's gives. It leaves what the game's function leaves that anything reads:
//    r3, the word at r1 - 16, and flush-to-zero off once a plane was tested (on when count is 0). Not the vector,
//    float and scratch registers it uses (volatile; its only caller, sub_822FA0A0, reads r3).
//    Checked phase as the other native functions: each call runs the game's function, keeps r3, the word and the FPCR
//    state, puts the word back, runs the native one and compares; the game goes on with the original's results.
//    After kPlaneChecked identical calls the native one runs alone (1 in kPlaneCheckEvery still checked); a
//    difference logs a MISMATCH and switches it off for the rest of the run.
//
// Cvars: bo2zm_native_entity_scan, bo2zm_native_plane_test (both on), and for bo2zm_benchmark_ab_s their A/B factors
// bo2zm_native_entity_scan_ab and bo2zm_native_plane_test_ab (1 or 2; 0 = not part of the A/B).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/intrinsics.h>  // simde and the generated code's vector helpers (the lvx byte order)
#include <rex/system/xmemory.h>

#include "bo2zm_game.h"
#include "bo2zm_benchmark.h"

REXCVAR_DEFINE_BOOL(bo2zm_native_entity_scan, true, "BO2",
                    "The game's entity loop (sub_822FD360): a native pass first reads the entities it will scan, so "
                    "their cache misses overlap (it only reads; the game's loop then runs unchanged). false = the "
                    "game's loop alone")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_entity_scan_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_native_entity_scan. 0 = not "
                     "part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_plane_test, true, "BO2",
                    "The game's box against planes test (sub_822FE3B8, per entity in its update) in native code, "
                    "checked against the game's own first. false = the game's code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_plane_test_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_native_plane_test. 0 = not "
                     "part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_822FD360);
REX_EXTERN(__imp__sub_822E6B78);
REX_EXTERN(__imp__sub_822FE3B8);

namespace {

using VI = simde__m128i;
using V = simde__m128;

// sub_822FD360's world and list.
constexpr uint32_t kList = 880;       // [world + 880]: the entity list
constexpr uint32_t kEntities = 672;   // [world + 672]: the entities, handle * 16 from there
constexpr uint32_t kRing = 48;        // the list's ring of u16 handles
constexpr uint32_t kRingMask = 0x3FF;
constexpr uint32_t kRingSlots = 1024;
constexpr uint32_t kScanAhead = 8;    // slots ahead whose entity is asked for while one is read
// sub_822FE3B8's constants (the addresses its code builds with lis/addi).
constexpr uint32_t kPlaneMask = BO2_GAME_ADDR(0x83C288C0, 0x837F2FC0);     // and-ed with the boxes (v63)
constexpr uint32_t kPlaneBias = BO2_GAME_ADDR(0x8205A2B0, 0x8204E600);     // subtracted from them (v62)
constexpr uint32_t kPlaneZero = BO2_GAME_ADDR(0x8205A2A0, 0x8204E5F0);     // the planes' components compared with it (v61)
constexpr uint32_t kPlaneOutside = BO2_GAME_ADDR(0x82000DC4, 0x82000C74);  // a dot below this float is outside (f0)
constexpr uint32_t kPlaneChecked = 20000;
constexpr uint32_t kPlaneCheckEvery = 1024;

inline const uint8_t* At(uint8_t* base, uint32_t a) {
  return rex::memory::GuestPtr<const uint8_t*>(base, a);
}
inline uint32_t Read32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, At(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint32_t Read16(uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, At(base, a), 2);
  return __builtin_bswap16(v);
}
inline uint32_t RawWord(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, At(base, a), 4);
  return v;
}
inline void SetRawWord(uint8_t* base, uint32_t a, uint32_t v) {
  std::memcpy(rex::memory::GuestPtr<uint8_t*>(base, a), &v, 4);
}

uint64_t NowNs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// A cvar and its A/B factor (AbArmCached: these run thousands of times a frame).
bool On(bool cvar, int factor) {
  if (factor != 0) {
    const int arm = bo2zm::benchmark::AbArmCached();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  return cvar;
}

// --- 1. The scan pass before sub_822FD360 ---------------------------------------------------------------------------

// The entity's class as sub_822FD360 works it out (0x822FD3E4-0x822FD440), for an entity with a def.
inline int32_t ClassOf(uint32_t def_flags, uint32_t at180, uint32_t at246) {
  const int32_t c = (def_flags & 0x30) ? 2 : ((at180 & 0xC00000) != 0 || at246 < 0x8000) ? 1 : 0;
  return (def_flags & 0x404) == 0x404 ? 3 : c;
}

// Reads what the game's loop will read of each entity in [first, first + count) and asks for the lines the entities
// of class wanted will need. Returns how many are of that class (which also keeps the loads).
uint32_t ScanPass(uint8_t* base, uint32_t world, uint32_t first, uint32_t count, int32_t wanted) {
  const uint32_t ring = Read32(base, world + kList) + kRing;
  const uint32_t entities = Read32(base, world + kEntities);
  auto entity = [&](uint32_t k) { return entities + (Read16(base, ring + 2 * ((first + k) & kRingMask)) << 4); };
  // The lines of the loop's first reads (e + 160 and e + 180 can be in two lines, e + 246 in a third).
  auto ask = [&](uint32_t e) {
    __builtin_prefetch(At(base, e + 160));
    __builtin_prefetch(At(base, e + 180));
    __builtin_prefetch(At(base, e + 246));
  };
  for (uint32_t k = 0; k < count && k < kScanAhead; ++k) {
    ask(entity(k));
  }
  uint32_t matched = 0;
  for (uint32_t k = 0; k < count; ++k) {
    if (k + kScanAhead < count) {
      ask(entity(k + kScanAhead));
    }
    const uint32_t e = entity(k);
    const uint32_t def = Read32(base, e + 160);
    const uint32_t def_flags = Read16(base, def != 0 ? def + 4 : e + 160);
    const uint32_t at180 = Read32(base, e + 180);
    const uint32_t at246 = Read16(base, e + 246);
    const bool match = def != 0 && ClassOf(def_flags, at180, at246) == wanted;
    // Of an entity of the class: the four lines from e + 48 (e + 48 .. e + 255 when the entity is 16-byte aligned).
    // Else the line just read, again, which costs nothing.
    const uint32_t from = match ? e + 48 : e + 160;
    const uint32_t step = match ? 64 : 0;
    for (uint32_t line = 0; line < 4; ++line) {
      __builtin_prefetch(At(base, from + line * step));
    }
    matched += match;
  }
  return matched;
}

struct ScanArm {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> entities{0};
  std::atomic<uint64_t> matched{0};
  std::atomic<uint64_t> ns{0};       // the whole call
  std::atomic<uint64_t> ns_pass{0};  // of it, the pass
  uint64_t calls_said = 0, entities_said = 0, matched_said = 0, ns_said = 0, ns_pass_said = 0;
};
ScanArm g_scan[2];  // [0] the game's loop alone, [1] the pass first

// Set by the hook of sub_822FD360 for the pass that its sub_822E6B78 call (its first) then runs.
struct PendingScan {
  bool active = false;
  uint32_t world = 0, first = 0, count = 0;
  int32_t wanted = 0;
  uint32_t matched = 0;
  uint64_t ns = 0;
};
thread_local PendingScan t_scan;

// --- 2. The plane test sub_822FE3B8 ---------------------------------------------------------------------------------

inline VI Reverse() { return simde_mm_loadu_si128(reinterpret_cast<const VI*>(rex::ppc::VectorMaskL)); }
// lvx128: the aligned 16 bytes, in the generated code's register byte order.
inline VI LoadAligned(uint8_t* base, uint32_t a) {
  return simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const VI*>(At(base, a & ~15u))), Reverse());
}
// lvlx128 a | lvrx128 a + 16: the 16 bytes at a, any alignment, in the same byte order.
inline VI LoadUnaligned(uint8_t* base, uint32_t a) {
  return simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const VI*>(At(base, a))), Reverse());
}
// vspltw128 v, 0 then stvewx128: the guest element 0 as a word.
inline uint32_t Element0(V v) {
  return uint32_t(simde_mm_cvtsi128_si32(simde_mm_shuffle_epi32(simde_mm_castps_si128(v), 0xFF)));
}

// sub_822FE3B8 natively (see the header). False: not taken (a denormal bound), nothing changed.
bool PlaneTest(PPCContext& ctx, uint8_t* base) {
  const uint32_t outside_bits = Read32(base, kPlaneOutside);
  if ((outside_bits & 0x7F800000u) == 0 && (outside_bits & 0x007FFFFFu) != 0) [[unlikely]] {
    return false;
  }
  float outside;
  std::memcpy(&outside, &outside_bits, 4);
  const uint32_t count = ctx.r4.u32;
  const VI mask = LoadAligned(base, kPlaneMask);
  const V bias = simde_mm_castsi128_ps(LoadAligned(base, kPlaneBias));
  const V box_r5 = simde_mm_castsi128_ps(simde_mm_and_si128(LoadUnaligned(base, ctx.r5.u32), mask));
  const V box_r6 = simde_mm_castsi128_ps(simde_mm_and_si128(LoadUnaligned(base, ctx.r6.u32), mask));
  ctx.fpscr.enableFlushMode();
  const VI corner_r5 = simde_mm_castps_si128(simde_mm_sub_ps(box_r5, bias));  // v63
  const VI corner_r6 = simde_mm_castps_si128(simde_mm_sub_ps(box_r6, bias));  // v62
  if (count == 0) {
    ctx.r3.u64 = 0;
    return true;
  }
  const V zero = simde_mm_castsi128_ps(LoadAligned(base, kPlaneZero));
  const uint32_t planes = ctx.r3.u32 + 16;
  uint32_t word = 0;
  uint64_t result = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const VI plane = LoadAligned(base, planes + 16 * i);
    const VI above = simde_mm_castps_si128(simde_mm_cmpgt_ps(simde_mm_castsi128_ps(plane), zero));
    const VI corner =
        simde_mm_or_si128(simde_mm_andnot_si128(above, corner_r5), simde_mm_and_si128(above, corner_r6));
    const V dot = simde_mm_dp_ps(simde_mm_castsi128_ps(plane), simde_mm_castsi128_ps(corner), 0xFF);
    word = Element0(dot);
    float d;
    std::memcpy(&d, &word, 4);
    if (double(d) < double(outside)) {
      result = 1;
      break;
    }
  }
  ctx.fpscr.disableFlushMode();
  const uint32_t at = (ctx.r1.u32 - 16) & ~3u;
  SetRawWord(base, at, __builtin_bswap32(word));
  ctx.r3.u64 = result;
  return true;
}

std::atomic<int> g_plane_phase{0};  // 0 checking, 1 native alone, 2 off
std::atomic<uint64_t> g_plane_calls{0};
std::atomic<uint64_t> g_plane_checked{0};
std::atomic<uint64_t> g_plane_ns_original{0};
std::atomic<uint64_t> g_plane_ns_native{0};
uint64_t g_plane_calls_said = 0;

void PlaneChecked(PPCContext& ctx, uint8_t* base) {
  const PPCContext input = ctx;
  const uint32_t at = (ctx.r1.u32 - 16) & ~3u;
  const uint32_t before = RawWord(base, at);
  const uint64_t t0 = NowNs();
  __imp__sub_822FE3B8(ctx, base);
  const uint64_t t1 = NowNs();
  const PPCContext game = ctx;
  const uint32_t word_game = RawWord(base, at);
  ctx = input;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  SetRawWord(base, at, before);
  const uint64_t t2 = NowNs();
  const bool taken = PlaneTest(ctx, base);
  const uint64_t t3 = NowNs();
  const uint32_t word_native = RawWord(base, at);
  if (taken) {
    if (ctx.r3.u64 != game.r3.u64 || word_native != word_game || ctx.fpscr.csr != game.fpscr.csr) {
      g_plane_phase.store(2, std::memory_order_relaxed);
      REXLOG_WARN("[native_entities] plane test (sub_822FE3B8): MISMATCH after {} checked calls: r3 {} / {}, word "
                  "at r1-16 {:08X} / {:08X}, FPCR {:08X} / {:08X} (game / native); planes {:08X}, count {}, boxes "
                  "{:08X} {:08X}. The native version is off for this session",
                  g_plane_checked.load(std::memory_order_relaxed), game.r3.u64, ctx.r3.u64,
                  __builtin_bswap32(word_game), __builtin_bswap32(word_native), game.fpscr.csr, ctx.fpscr.csr,
                  input.r3.u32, input.r4.u32, input.r5.u32, input.r6.u32);
    } else {
      g_plane_ns_original.fetch_add(t1 - t0, std::memory_order_relaxed);
      g_plane_ns_native.fetch_add(t3 - t2, std::memory_order_relaxed);
      const uint64_t n = g_plane_checked.fetch_add(1, std::memory_order_relaxed) + 1;
      int expected = 0;
      if (n >= kPlaneChecked && g_plane_phase.compare_exchange_strong(expected, 1)) {
        REXLOG_INFO("[native_entities] plane test (sub_822FE3B8): {} checked calls identical: the native version "
                    "runs alone now (1 in {} still checked)",
                    n, kPlaneCheckEvery);
      }
    }
  }
  // The game goes on with its own function's results.
  ctx = game;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  SetRawWord(base, at, word_game);
}

// --- The report -----------------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_next_report{0};

void Report() {
  const uint64_t now = NowNs();
  uint64_t next = g_next_report.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_report.compare_exchange_strong(next, now + 10'000'000'000ull);
    return;
  }
  if (now < next || !g_next_report.compare_exchange_strong(next, now + 10'000'000'000ull)) {
    return;
  }
  const char* names[2] = {"the game's loop alone", "with the pass"};
  for (int i = 0; i < 2; ++i) {
    ScanArm& a = g_scan[i];
    const uint64_t calls = a.calls.load(std::memory_order_relaxed);
    const uint64_t entities = a.entities.load(std::memory_order_relaxed);
    const uint64_t matched = a.matched.load(std::memory_order_relaxed);
    const uint64_t ns = a.ns.load(std::memory_order_relaxed);
    const uint64_t ns_pass = a.ns_pass.load(std::memory_order_relaxed);
    const double c = double(calls - a.calls_said);
    if (c > 0) {
      const double e = double(entities - a.entities_said);
      REXLOG_INFO("[native_entities] entity scan (sub_822FD360), {}: {} calls in 10 s, {:.1f} entities a call{}; "
                  "{:.2f} us a call ({:.1f} ns an entity){}",
                  names[i], uint64_t(c), e / c,
                  i == 1 ? fmt::format(" ({:.1f} of its class)", double(matched - a.matched_said) / c) : "",
                  double(ns - a.ns_said) / c / 1e3, e > 0 ? double(ns - a.ns_said) / e : 0.0,
                  i == 1 ? fmt::format(", of it the pass {:.2f} us", double(ns_pass - a.ns_pass_said) / c / 1e3) : "");
    }
    a.calls_said = calls;
    a.entities_said = entities;
    a.matched_said = matched;
    a.ns_said = ns;
    a.ns_pass_said = ns_pass;
  }
  const uint64_t calls = g_plane_calls.load(std::memory_order_relaxed);
  const uint64_t checked = g_plane_checked.load(std::memory_order_relaxed);
  const int phase = g_plane_phase.load(std::memory_order_relaxed);
  const double us_original = checked ? double(g_plane_ns_original.load(std::memory_order_relaxed)) / checked / 1e3 : 0;
  const double us_native = checked ? double(g_plane_ns_native.load(std::memory_order_relaxed)) / checked / 1e3 : 0;
  REXLOG_INFO("[native_entities] plane test (sub_822FE3B8): {} calls in 10 s, {} checked since start, phase {}; per "
              "checked call {:.3f} us the game's, {:.3f} us native ({:.1f}x)",
              calls - g_plane_calls_said, checked,
              phase == 0 ? "checking" : phase == 1 ? "native" : "OFF (difference)", us_original, us_native,
              us_native > 0 ? us_original / us_native : 0.0);
  g_plane_calls_said = calls;
}

}  // namespace

REX_HOOK_RAW(sub_822FD360) {
  static const bool cvar = REXCVAR_GET(bo2zm_native_entity_scan);
  static const int factor = REXCVAR_GET(bo2zm_native_entity_scan_ab);
  const bool pass = On(cvar, factor);
  const uint32_t count = std::min<uint32_t>(ctx.r5.u32 - ctx.r4.u32, kRingSlots);
  ScanArm& arm = g_scan[pass ? 1 : 0];
  t_scan = PendingScan{pass && count != 0, ctx.r3.u32, ctx.r4.u32, count, ctx.r6.s32, 0, 0};
  const uint64_t t0 = NowNs();
  __imp__sub_822FD360(ctx, base);
  const uint64_t t1 = NowNs();
  t_scan.active = false;
  arm.calls.fetch_add(1, std::memory_order_relaxed);
  arm.entities.fetch_add(count, std::memory_order_relaxed);
  arm.matched.fetch_add(t_scan.matched, std::memory_order_relaxed);
  arm.ns.fetch_add(t1 - t0, std::memory_order_relaxed);
  arm.ns_pass.fetch_add(t_scan.ns, std::memory_order_relaxed);
  Report();
}

// sub_822E6B78(world r3): one more reader of the list (other readers call it too: they find no pass pending).
REX_HOOK_RAW(sub_822E6B78) {
  __imp__sub_822E6B78(ctx, base);
  if (t_scan.active) [[unlikely]] {
    t_scan.active = false;
    const uint64_t t0 = NowNs();
    t_scan.matched = ScanPass(base, t_scan.world, t_scan.first, t_scan.count, t_scan.wanted);
    t_scan.ns = NowNs() - t0;
  }
}

REX_HOOK_RAW(sub_822FE3B8) {
  static const bool cvar = REXCVAR_GET(bo2zm_native_plane_test);
  static const int factor = REXCVAR_GET(bo2zm_native_plane_test_ab);
  const int phase = g_plane_phase.load(std::memory_order_relaxed);
  if (phase == 2 || !On(cvar, factor)) {
    __imp__sub_822FE3B8(ctx, base);
    return;
  }
  const uint64_t call = g_plane_calls.fetch_add(1, std::memory_order_relaxed);
  if (phase == 1 && call % kPlaneCheckEvery != 0) [[likely]] {
    if (!PlaneTest(ctx, base)) {
      __imp__sub_822FE3B8(ctx, base);
    }
    return;
  }
  PlaneChecked(ctx, base);
}
