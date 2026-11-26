// A native version of the game's bounds-tree frustum culling (bo2zm_native_cull), verified against the original in a
// checked phase before it runs on its own.
//
// What the game does
// ------------------
// sub_828F6F08(node r3, planes r4, r5, r6, r7) returns r5 and marks visible items. Planes: 32-byte entries (the plane,
// then a permute control that picks each component of the box's two corners), the count at planes + 512. A node:
// two corners as unaligned 16-byte loads at +0 and +12, the child count (u16) at +24, children at node + [node + 36]
// (40 bytes each), its own items in two lists: A (count +30, u16 indices at [node + 32]) and B (count +26, u16
// indices from [world + 80] + [node + 28] * 2; world = [0x84916250]). For a node it:
//   1. if r6 != 0, calls sub_828F6CB8(node, &r6 in its caller's frame, r7): r18 = its result, r19 = the updated r6;
//      returns at once when r18 is 0;
//   2. for each plane: n = dot(plane, perm(cornerA, cornerB, control)), p = dot(plane, perm(cornerB, cornerA,
//      control)); keeps the plane (a compacted copy in its frame) when constant > p; minimum n over the planes;
//      returns when that minimum is not above the float at 0x82000DC4 (outside);
//   3. no plane kept and r19 == 0: every item of both lists is visible;
//   4. else, with children: each child with the kept planes (r5 = 0, r6 = r19, r7 = r18);
//   5. else, each item not yet visible whose box's minimum over the kept planes of dot(plane, perm(corner +0, corner
//      +12, control)) is above a constant, and for which sub_828F6B78(item box, r19, r18) returns 0, is marked visible.
// Visible = byte 1 in two arrays (A, B) whose pointers the culling job (sub_828F7770) puts in the thread's block at
// [r13 + 0] + 6776 and + 6780: the view's arrays in the world, shared by the workers culling other parts of the tree.
// Items' boxes: A at [world + 84] + index * 36, B at [world + 88] + index * 96 + 72. r6 and r7 are an occluder count
// and list (80 bytes each: five planes). sub_828F6B78(box, count, occluders) is a pure test: 1 when the box is inside
// one of the occluders (it relies on its caller's r6 surviving the call: it writes only r3, r5, r7-r11 and vector
// registers). sub_828F6CB8(node, &count, occluders) returns 0 when the node is inside an occluder, else the list,
// after dropping (swap with the last, count - 1) the occluders the node is entirely outside of: it reorders the list
// in place, which only changes the order in which the item tests go through it. On the PC this function was the
// game's largest CPU cost after the allocator search: 30-47 ms per second across the job workers and the Backend.
//
// Natively: the same vector operations the generated code performs (simde dp_ps, min_ps, cmpgt_ps, the generated
// code's vperm helper, its byte order for vector loads, flush-to-zero around them), without the register file and
// the per-load byte shuffles; the recursion stays native; the two game functions are called as the game's code calls
// them (registers, frame, return address). A localized copy of the recompiled code (tools/localize.py) was tried
// first and turned out slower.
//
// The guard. Snapshotting the arrays would not work: other workers write them at the same time, and a restore could
// undo their marks. Instead, a checked call points the thread's two array pointers at a private zeroed pair, runs the
// game's function, keeps the arrays, zeroes them again and runs the native version on the same input; the arrays and
// r3 must be equal. Then the pointers are put back and the game's function runs for real. The first kChecked top-level
// calls are checked, then one in kCheckEvery; a difference logs a MISMATCH and turns the native version off for the
// rest of the run. The item indices are 16-bit, so 64 KB per array covers them.
//
// Speed: on the Switch, Item took 14 % of a core and Cull 5.8 %, the largest self time of any game thread. A third of
// Item's samples waited on the first load of the item's box (scattered through the world's arrays, mostly not in the
// cache) and on its visible byte. The leaf loop therefore prefetches the box and the byte of the item kPrefetchAhead
// places ahead while it tests the current one (bo2zm_native_cull_prefetch): that changes when memory arrives, not what
// is computed. Each plane's permute (the generated code's vperm: an index and a select mask derived from the plane's
// control) is also prepared once per plane instead of once per plane and item: the same operations on the same values,
// so the same result, and the check above still compares against the game's function.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "bo2zm_game.h"
#include "bo2zm_benchmark.h"
#include <rex/logging.h>
#include <rex/ppc/intrinsics.h>  // simde and the generated code's vector helpers (vperm, the lvx byte order)
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(bo2zm_native_cull, true, "BO2",
                    "The game's bounds-tree culling (sub_828F6F08) in native code, checked against the game's own "
                    "first (bo2zm_native_cull.cpp). false = the game's code only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_cull_prefetch, true, "BO2",
                    "Native culling: ask for the next items' boxes while testing one (it only changes when memory "
                    "arrives). false = no prefetch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_cull_prefetch_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_cull_prefetch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_828F6F08);
REX_EXTERN(__imp__sub_828F6CB8);
REX_EXTERN(__imp__sub_828F6B78);

namespace {

using Clock = std::chrono::steady_clock;
using VI = simde__m128i;
using V = simde__m128;

constexpr uint32_t kWorld = BO2_GAME_ADDR(0x84916250, 0x841382D0);        // [kWorld] = the world
constexpr uint32_t kConstMin = BO2_GAME_ADDR(0x8205A2C0, 0x8204E610);     // the minimum's start (v63)
constexpr uint32_t kConstPerm = BO2_GAME_ADDR(0x83C28720, 0x837F2E20);    // the corners' permute (v7)
constexpr uint32_t kConstMask = BO2_GAME_ADDR(0x83C287C0, 0x837F2EC0);    // the "plane kept" word (v59 in the plane loop)
constexpr uint32_t kConstKeep = BO2_GAME_ADDR(0x8205A2A0, 0x8204E5F0);    // compared with the planes' far dots (v55 / v38)
constexpr uint32_t kOutside = BO2_GAME_ADDR(0x82000DC4, 0x82000C74);      // float: the minimum must be above it
constexpr uint32_t kFrame = 768;               // sub_828F6F08's stack frame
constexpr uint32_t kMaxPlanes = 16;            // its frame holds 16 kept planes (r1 + 96 .. r1 + 608)
constexpr uint32_t kLrState = BO2_GAME_ADDR(0x828F6F58, 0x826D8310);      // return addresses of its calls
constexpr uint32_t kLrItemA = BO2_GAME_ADDR(0x828F7238, 0x826D85F0);
constexpr uint32_t kLrItemB = BO2_GAME_ADDR(0x828F73C0, 0x826D8778);
constexpr uint32_t kArrayA = 6776;  // offsets in the thread's block ([r13 + 0])
constexpr uint32_t kArrayB = 6780;
constexpr uint32_t kArrayBytes = 0x10000;  // 16-bit item indices
constexpr uint64_t kChecked = 2000;
constexpr uint64_t kCheckEvery = 1024;
constexpr uint32_t kPrefetchAhead = 4;  // items ahead in a leaf's lists

enum Mode : int { kNone, kOriginal };
thread_local int t_mode = kNone;  // inside a checked call's game run: its recursion is the game's too

std::atomic<int> g_phase{0};  // 0 checking, 1 native alone, 2 off
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_checked{0};
std::atomic<uint64_t> g_ns_original{0};
std::atomic<uint64_t> g_ns_native{0};
std::atomic<uint64_t> g_timed{0};
std::atomic<int64_t> g_last_report{0};
std::mutex g_check_mutex;  // the scratch arrays are shared by the checks
uint32_t g_scratch = 0;

inline uint32_t Read32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
inline uint16_t Read16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return __builtin_bswap16(v);
}
inline void Write32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
inline uint64_t NsSince(Clock::time_point t) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
}

inline VI Reverse() { return simde_mm_loadu_si128(reinterpret_cast<const VI*>(rex::ppc::VectorMaskL)); }
// lvx128: the aligned 16 bytes, in the generated code's register byte order.
inline VI LoadAligned(const uint8_t* base, uint32_t a) {
  return simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const VI*>(base + (a & ~15u))), Reverse());
}
// lvlx128 a | lvrx128 a + 16: the 16 bytes at a, any alignment, in the same byte order.
inline VI LoadUnaligned(const uint8_t* base, uint32_t a) {
  return simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const VI*>(base + a)), Reverse());
}
// vspltw128 v, 0 then stvewx128 and lwz: the guest element 0 as a word.
inline uint32_t Element0(VI v) {
  return uint32_t(simde_mm_cvtsi128_si32(simde_mm_shuffle_epi32(v, 0xFF)));
}
inline float Element0F(V v) {
  const uint32_t w = Element0(simde_mm_castps_si128(v));
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

// rex::ppc::simde_mm_perm_epi8_(a, b, c) in two halves: what depends only on c (prepared once per plane), and the
// blend of a and b (per box). The same simde operations on the same values as the helper.
struct Perm {
  VI index;   // 0xF - (c & 0xF)
  VI select;  // c << 3 (each 32-bit lane): blendv takes b where its byte's top bit is set
};
inline Perm PreparePerm(VI c) {
  const VI d = simde_mm_set1_epi8(0xF);
  return {simde_mm_sub_epi8(d, simde_mm_and_si128(c, d)), simde_mm_slli_epi32(c, 3)};
}
inline VI ApplyPerm(VI a, VI b, const Perm& p) {
  return simde_mm_blendv_epi8(simde_mm_shuffle_epi8(a, p.index), simde_mm_shuffle_epi8(b, p.index), p.select);
}

struct Plane {
  VI plane;
  VI control;
  Perm perm;  // of control
};

struct Consts {
  VI min_start;
  VI perm;
  Perm perm_corner;  // of perm
  VI mask;
  VI keep;
  float outside;
};

struct Native {
  PPCContext& ctx;
  uint8_t* base;
  uint32_t array_a;
  uint32_t array_b;
  Consts k;
  bool overflow = false;  // more kept planes than the game's frame holds: give up (the game's function runs)
  bool prefetch = true;   // bo2zm_native_cull_prefetch (and its A/B)
};

// The box (28 bytes from box: the loads at +0 and +12) and the visible byte of an item tested soon.
inline void PrefetchItem(const uint8_t* base, uint32_t box, uint32_t visible) {
  __builtin_prefetch(base + box);
  __builtin_prefetch(base + box + 27);
  __builtin_prefetch(base + visible);
}

bool PrefetchOn() {
  static const bool on = REXCVAR_GET(bo2zm_native_cull_prefetch);
  static const bool ab = REXCVAR_GET(bo2zm_native_cull_prefetch_ab);
  if (ab) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & 1) != 0;
    }
  }
  return on;
}

void Cull(Native& n, uint32_t node, const Plane* planes, uint32_t count, uint32_t r6, uint32_t r7, uint32_t r1);

// Marks the items of a list visible (step 3).
void MarkAll(Native& n, uint32_t node) {
  uint8_t* base = n.base;
  const uint32_t count_a = Read16(base, node + 30);
  if (count_a) {
    const uint32_t list = Read32(base, node + 32);
    for (uint32_t i = 0; i < count_a; ++i) {
      base[n.array_a + Read16(base, list + 2 * i)] = 1;
    }
  }
  const uint32_t count_b = Read16(base, node + 26);
  if (count_b) {
    const uint32_t world = Read32(base, kWorld);
    const uint32_t list = Read32(base, world + 80) + uint32_t(Read16(base, node + 28)) * 2;
    for (uint32_t i = 0; i < count_b; ++i) {
      base[n.array_b + Read16(base, list + 2 * i)] = 1;
    }
  }
}

// One item of a leaf (step 5): its box against the kept planes, then the game's test.
void Item(Native& n, uint32_t box, uint32_t array, uint32_t index, const Plane* planes, uint32_t count, uint32_t r19,
          uint32_t r18, uint32_t r1, uint32_t lr) {
  uint8_t* base = n.base;
  PPCContext& ctx = n.ctx;
  if (base[array + index] != 0) {
    return;
  }
  const VI corner_a = ApplyPerm(LoadUnaligned(base, box), n.k.min_start, n.k.perm_corner);
  const VI corner_b = ApplyPerm(LoadUnaligned(base, box + 12), n.k.min_start, n.k.perm_corner);
  V minimum = simde_mm_castsi128_ps(n.k.min_start);
  for (uint32_t i = 0; i < count; ++i) {
    const VI corner = ApplyPerm(corner_a, corner_b, planes[i].perm);
    ctx.fpscr.enableFlushMode();
    const V dot = simde_mm_dp_ps(simde_mm_castsi128_ps(planes[i].plane), simde_mm_castsi128_ps(corner), 0xFF);
    minimum = simde_mm_min_ps(minimum, dot);
  }
  ctx.fpscr.enableFlushMode();
  const V above = simde_mm_cmpgt_ps(minimum, simde_mm_castsi128_ps(n.k.keep));
  if (Element0(simde_mm_castps_si128(above)) == 0) {
    return;
  }
  ctx.r3.u64 = box;
  ctx.r4.u64 = r19;
  ctx.r5.u64 = r18;
  ctx.r1.u64 = r1;
  ctx.lr = lr;
  __imp__sub_828F6B78(ctx, base);
  if ((ctx.r3.u32 & 0xFF) == 0) {
    base[array + index] = 1;
  }
}

// One node (sub_828F6F08's body). r1 is the guest stack pointer at its entry.
void Cull(Native& n, uint32_t node, const Plane* planes, uint32_t count, uint32_t r6, uint32_t r7, uint32_t r1) {
  uint8_t* base = n.base;
  PPCContext& ctx = n.ctx;
  const uint32_t frame = r1 - kFrame;
  uint32_t r19 = r6;
  uint32_t r18 = r7;
  Write32(base, r1 + 44, r6);  // stw r6,812(r1): the caller's frame
  if (r6 != 0) {
    ctx.r3.u64 = node;
    ctx.r4.u64 = r1 + 44;
    ctx.r5.u64 = r7;
    ctx.r1.u64 = frame;
    ctx.lr = kLrState;
    __imp__sub_828F6CB8(ctx, base);
    r19 = Read32(base, r1 + 44);
    r18 = ctx.r3.u32;
  }
  if (r18 == 0) {
    return;
  }
  // Step 2: the planes.
  const VI corner_a = ApplyPerm(LoadUnaligned(base, node), n.k.min_start, n.k.perm_corner);       // v61
  const VI corner_b = ApplyPerm(LoadUnaligned(base, node + 12), n.k.min_start, n.k.perm_corner);  // v60
  Plane kept[kMaxPlanes];
  uint32_t kept_count = 0;
  V minimum = simde_mm_castsi128_ps(n.k.min_start);
  for (uint32_t i = 0; i < count; ++i) {
    const VI far_corner = ApplyPerm(corner_b, corner_a, planes[i].perm);   // v56
    const VI near_corner = ApplyPerm(corner_a, corner_b, planes[i].perm);  // v54
    ctx.fpscr.enableFlushMode();
    const V plane = simde_mm_castsi128_ps(planes[i].plane);
    const V far_dot = simde_mm_dp_ps(plane, simde_mm_castsi128_ps(far_corner), 0xFF);
    const V near_dot = simde_mm_dp_ps(plane, simde_mm_castsi128_ps(near_corner), 0xFF);
    if (kept_count >= kMaxPlanes) {
      n.overflow = true;
      return;
    }
    kept[kept_count] = planes[i];  // stored at the kept count; kept only if the word below is not 0
    const VI straddles =
        simde_mm_and_si128(simde_mm_castps_si128(simde_mm_cmpgt_ps(simde_mm_castsi128_ps(n.k.keep), far_dot)), n.k.mask);
    minimum = simde_mm_min_ps(minimum, near_dot);
    kept_count = (kept_count + Element0(straddles)) & 0xFFFF;
  }
  ctx.fpscr.disableFlushMode();
  if (!(double(Element0F(minimum)) > double(n.k.outside))) {
    return;  // outside
  }
  if (kept_count > kMaxPlanes) {
    n.overflow = true;
    return;
  }
  if (kept_count == 0 && r19 == 0) {
    MarkAll(n, node);  // step 3
    return;
  }
  const uint32_t children = Read16(base, node + 24);
  if (children != 0) {  // step 4
    uint32_t child = node + Read32(base, node + 36);
    for (uint32_t i = 0; i < children && !n.overflow; ++i, child += 40) {
      Cull(n, child, kept, kept_count, r19, r18, frame);
    }
    return;
  }
  // Step 5: the items of the leaf.
  const uint32_t count_a = Read16(base, node + 30);
  if (count_a) {
    const uint32_t world = Read32(base, kWorld);
    const uint32_t boxes = Read32(base, world + 84);
    const uint32_t list = Read32(base, node + 32);
    if (n.prefetch) {
      for (uint32_t i = 0; i < count_a && i < kPrefetchAhead; ++i) {
        const uint32_t index = Read16(base, list + 2 * i);
        PrefetchItem(base, boxes + index * 36, n.array_a + index);
      }
    }
    for (uint32_t i = 0; i < count_a; ++i) {
      if (n.prefetch && i + kPrefetchAhead < count_a) {
        const uint32_t ahead = Read16(base, list + 2 * (i + kPrefetchAhead));
        PrefetchItem(base, boxes + ahead * 36, n.array_a + ahead);
      }
      const uint32_t index = Read16(base, list + 2 * i);
      Item(n, boxes + index * 36, n.array_a, index, kept, kept_count, r19, r18, frame, kLrItemA);
    }
  }
  const uint32_t count_b = Read16(base, node + 26);
  if (count_b) {
    const uint32_t world = Read32(base, kWorld);
    const uint32_t list = Read32(base, world + 80) + uint32_t(Read16(base, node + 28)) * 2;
    const uint32_t boxes = Read32(base, world + 88);
    if (n.prefetch) {
      for (uint32_t i = 0; i < count_b && i < kPrefetchAhead; ++i) {
        const uint32_t index = Read16(base, list + 2 * i);
        PrefetchItem(base, boxes + index * 96 + 72, n.array_b + index);
      }
    }
    for (uint32_t i = 0; i < count_b; ++i) {
      if (n.prefetch && i + kPrefetchAhead < count_b) {
        const uint32_t ahead = Read16(base, list + 2 * (i + kPrefetchAhead));
        PrefetchItem(base, boxes + ahead * 96 + 72, n.array_b + ahead);
      }
      const uint32_t index = Read16(base, list + 2 * i);
      Item(n, boxes + index * 96 + 72, n.array_b, index, kept, kept_count, r19, r18, frame, kLrItemB);
    }
  }
}

// The whole function natively; false when it must give up (the game's function then runs).
bool NativeTop(PPCContext& ctx, uint8_t* base) {
  const uint32_t block = Read32(base, ctx.r13.u32);
  Native n{ctx, base, Read32(base, block + kArrayA), Read32(base, block + kArrayB), {}};
  n.k.min_start = LoadAligned(base, kConstMin);
  n.k.perm = LoadAligned(base, kConstPerm);
  n.k.perm_corner = PreparePerm(n.k.perm);
  n.prefetch = PrefetchOn();
  n.k.mask = LoadAligned(base, kConstMask);
  n.k.keep = LoadAligned(base, kConstKeep);
  const uint32_t outside = Read32(base, kOutside);
  std::memcpy(&n.k.outside, &outside, 4);
  const uint32_t planes_at = ctx.r4.u32;
  const uint32_t count = Read32(base, planes_at + 512);
  if (count > kMaxPlanes) {
    return false;
  }
  Plane planes[kMaxPlanes];
  for (uint32_t i = 0; i < count; ++i) {
    planes[i].plane = LoadAligned(base, planes_at + 32 * i);
    planes[i].control = LoadAligned(base, planes_at + 32 * i + 16);
    planes[i].perm = PreparePerm(planes[i].control);
  }
  // What the game's function reads at its entry (for a fallback after an overflow) and leaves on its return.
  const uint64_t r1 = ctx.r1.u64, r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64,
                 r7 = ctx.r7.u64, lr = ctx.lr;
  Cull(n, uint32_t(r3), planes, count, uint32_t(r6), uint32_t(r7), uint32_t(r1));
  ctx.r1.u64 = r1;
  ctx.lr = lr;
  if (n.overflow) [[unlikely]] {
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    return false;
  }
  ctx.r3.u64 = r5;  // the game's function returns its r5
  return true;
}

void Report() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  int64_t last = g_last_report.load(std::memory_order_relaxed);
  if (last != 0 && now - last < 10000) {
    return;
  }
  if (!g_last_report.compare_exchange_strong(last, now, std::memory_order_relaxed) || last == 0) {
    return;
  }
  const uint64_t n = g_timed.load(std::memory_order_relaxed);
  const double us_original = n ? double(g_ns_original.load(std::memory_order_relaxed)) / double(n) / 1e3 : 0.0;
  const double us_native = n ? double(g_ns_native.load(std::memory_order_relaxed)) / double(n) / 1e3 : 0.0;
  const int phase = g_phase.load(std::memory_order_relaxed);
  REXLOG_INFO("[native_cull] top-level calls {} ({} checked, all identical so far), phase {}; per checked call "
              "{:.2f} us the game's, {:.2f} us native ({:.1f}x)",
              g_calls.load(std::memory_order_relaxed), g_checked.load(std::memory_order_relaxed),
              phase == 0 ? "checking" : phase == 1 ? "native" : "OFF", us_original, us_native,
              us_native > 0 ? us_original / us_native : 0.0);
}

void RunOriginal(PPCContext& ctx, uint8_t* base) {
  t_mode = kOriginal;
  __imp__sub_828F6F08(ctx, base);
  t_mode = kNone;
}

// One checked top-level call (see the header comment).
void Checked(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_check_mutex);
  if (g_scratch == 0) {
    g_scratch = rex::Runtime::instance()->memory()->SystemHeapAlloc(2 * kArrayBytes);
    if (g_scratch == 0) {
      REXLOG_ERROR("[native_cull] no guest memory for the check arrays: the native version is off");
      g_phase.store(2, std::memory_order_relaxed);
      RunOriginal(ctx, base);
      return;
    }
  }
  const uint32_t block = Read32(base, ctx.r13.u32);
  const uint32_t real_a = Read32(base, block + kArrayA);
  const uint32_t real_b = Read32(base, block + kArrayB);
  const PPCContext input = ctx;
  std::memset(base + g_scratch, 0, 2 * kArrayBytes);
  Write32(base, block + kArrayA, g_scratch);
  Write32(base, block + kArrayB, g_scratch + kArrayBytes);
  const auto t0 = Clock::now();
  RunOriginal(ctx, base);
  const uint64_t ns_original = NsSince(t0);
  const uint64_t r3_original = ctx.r3.u64;
  static std::vector<uint8_t> marks_original;  // under g_check_mutex
  marks_original.assign(base + g_scratch, base + g_scratch + 2 * kArrayBytes);
  ctx = input;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  std::memset(base + g_scratch, 0, 2 * kArrayBytes);
  const auto t1 = Clock::now();
  const bool native = NativeTop(ctx, base);
  const uint64_t ns_native = NsSince(t1);
  const uint64_t r3_native = ctx.r3.u64;
  Write32(base, block + kArrayA, real_a);
  Write32(base, block + kArrayB, real_b);
  if (native) {
    const bool same = r3_native == r3_original &&
                      std::memcmp(marks_original.data(), base + g_scratch, 2 * kArrayBytes) == 0;
    if (!same) {
      g_phase.store(2, std::memory_order_relaxed);
      uint32_t at = 0;
      while (at < 2 * kArrayBytes && marks_original[at] == base[g_scratch + at]) {
        ++at;
      }
      REXLOG_ERROR("[native_cull] MISMATCH after {} checked calls: {} (array byte {} of {}: game {} native {}); node "
                   "{:08X} planes {:08X} r6 {:08X} r7 {:08X}. The native version is off for the session",
                   g_checked.load(std::memory_order_relaxed),
                   r3_native != r3_original ? "r3 differs" : "the visible items differ", at, 2 * kArrayBytes,
                   at < 2 * kArrayBytes ? marks_original[at] : 0, at < 2 * kArrayBytes ? base[g_scratch + at] : 0,
                   input.r3.u32, input.r4.u32, input.r6.u32, input.r7.u32);
    } else {
      g_ns_original.fetch_add(ns_original, std::memory_order_relaxed);
      g_ns_native.fetch_add(ns_native, std::memory_order_relaxed);
      g_timed.fetch_add(1, std::memory_order_relaxed);
      const uint64_t n = g_checked.fetch_add(1, std::memory_order_relaxed) + 1;
      int expected = 0;
      if (n >= kChecked && g_phase.compare_exchange_strong(expected, 1)) {
        REXLOG_INFO("[native_cull] {} checked calls identical: the native version runs alone now (1 in {} still "
                    "checked)",
                    n, kCheckEvery);
      }
    }
  }
  // For real: the game's function on the view's arrays.
  ctx = input;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  RunOriginal(ctx, base);
}

}  // namespace

REX_HOOK_RAW(sub_828F6F08) {
  if (t_mode == kOriginal) {  // recursion inside a game run
    __imp__sub_828F6F08(ctx, base);
    return;
  }
  static const bool on = REXCVAR_GET(bo2zm_native_cull);
  const int phase = g_phase.load(std::memory_order_relaxed);
  if (!on || phase == 2) {
    __imp__sub_828F6F08(ctx, base);
    return;
  }
  const uint64_t call = g_calls.fetch_add(1, std::memory_order_relaxed);
  if (phase == 1 && call % kCheckEvery != 0) {
    if (!NativeTop(ctx, base)) {
      RunOriginal(ctx, base);
    }
  } else {
    Checked(ctx, base);
  }
  if ((call & 1023) == 0) {
    Report();
  }
}
