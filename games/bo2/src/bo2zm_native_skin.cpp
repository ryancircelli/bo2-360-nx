// Vector rewrites of BO2's CPU vertex skinning kernels, from their PowerPC code (TU18 default_mp.xex):
// sub_82912D78 (one bone per vertex), sub_82913378 (two) and sub_82913B50 (three). The job workers and the Backend
// call each of them about 330 times a second in a Town match, at 78-209 us per call on the Switch even at its highest
// clocks: about 13 % of a core in all. The recompiled code runs every VMX128 instruction through the emulated register
// file, and localized copies (tools/localize.py) were only 1.2x faster. These rewrites keep each vertex in vector
// registers instead.
//
// What the three kernels do, per vertex (r3 the vertices, 24 bytes each; r4 the bone records; r5 the count; r6 the
// bone matrices; r7 the output, 32 bytes each; r8 the position scale and bias):
//   - position: four signed 16-bit integers at +0 (vupkhsb128 of the first 8 bytes, vcsxwfp128), times the scale
//     plus the bias;
//   - two 2:10:10:10 words at +16 and +20 (vupkd3d128 type 8, then times K11 plus K10): call them T and N;
//   - the matrix: one bone's four rows, or the rows blended by the weights (16-bit fractions, made with lvlx, a
//     vperm and vcuxwfp128 ..., 16): two bones (1 - w) * A + w * B, three bones (1 - wa - wb) * C + wa * A + wb * B;
//   - the position transformed (z * row 2 + row 3, then y and x), T and N rotated (z * row 2, then y and x), both
//     times K8 plus K9 and packed back (vpkd3d128 type 2);
//   - out +0: the transformed position, with the original w put in by a vperm; out +16: the vertex's bytes 8-15 as
//     they were, then T and N packed.
//
// Exactly the same operations as the recompiled code, so the checked phase compares them bit for bit. On the PC
// (x86-64-v2, no FMA) vmaddfp is a multiply and an add. On the Switch GCC turns the recompiled add(mul()) into one
// fused fmla (12, 16 and 20 fmla for the 12, 16 and 20 vmaddfp of the three compiled kernels), and where a vmaddfp
// adds a product that a vmulfp made, GCC fuses one of the two products and rounds the other. Which one it fuses, read
// from the compiled kernels (the operands of their fmul), is reproduced in Chain and in the blends below. A different
// choice changes the last bit, and the transform's cancellations can grow that to tens of units (seen on the two-bone
// kernel). Vectors are kept with lane i = PowerPC element i; the byte-level steps (lvlx, vperm) work on the bytes in
// memory order, as the PowerPC does.
//
// The constant vectors are the game's, read from guest memory at each call (the addresses its lis/addi pairs form).
// A vertex array that is not 8-byte aligned runs the game's own function: its lvlx/lvrx pairs only read the first 8
// bytes of a vertex correctly when it is. Vertices go two at a time (the work of one is a chain of dependent
// multiply-adds), and a vertex whose bone record is the previous one's reuses its matrix.
//
// The fast form (bo2zm_native_skin_fast) gives the same bits with fewer instructions: T and N unpacked in
// vector registers from the vertex's bytes 8-23 (already loaded) instead of scalar code and a trip through memory,
// both packed with two pairwise adds instead of a horizontal add each, a weight converted as the 16-bit integer it is
// (the game's vperm puts it in the low half of each word, so vcuxwfp128 ..., 16 is exactly that integer times 2^-16),
// and the position's w put in with a lane move instead of a table lookup. The last two depend on the game's two
// vperm controls, which it fills at run time (0x84BF3540 and 0x83C28900 are zero in the image): each call checks
// them, and any other content runs the general form. At round 15 the three kernels run about 960 times a second
// each, at ~300 us per mesh: a quarter of a core.

#include "bo2zm_game.h"
#include "bo2zm_native_skin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>  // kPack2101010_*
#include <rex/system/xmemory.h>
#include <simde/arm/neon.h>

#include "bo2zm_benchmark.h"
#include "bo2zm_gpu_skin.h"

REXCVAR_DEFINE_BOOL(bo2zm_native_skin_fast, true, "BO2",
                    "The skinning rewrites' fast form (the same results with fewer instructions, see "
                    "bo2zm_native_skin.cpp). false = the general form");
REXCVAR_DEFINE_INT32(bo2zm_native_skin_fast_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_native_skin_fast (on = the "
                     "fast form, off = the general form). 0 = not part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82912D78);
REX_EXTERN(__imp__sub_82913378);
REX_EXTERN(__imp__sub_82913B50);

namespace bo2zm::native_skin {
namespace {

using F4 = simde_float32x4_t;
using U4 = simde_uint32x4_t;
using S4 = simde_int32x4_t;
using B16 = simde_uint8x16_t;

// The constant vectors (lis/addi in the kernels' setup).
constexpr uint32_t kPermPosition = BO2_GAME_ADDR(0x83C28900, 0x837F3000);  // vperm control: the transformed position with the original w
constexpr uint32_t kK8 = BO2_GAME_ADDR(0x8205A780, 0x8204EAD0);            // T and N after the rotation: times K8 plus K9
constexpr uint32_t kK9 = BO2_GAME_ADDR(0x8205A790, 0x8204EAE0);
constexpr uint32_t kK11 = BO2_GAME_ADDR(0x8205A7A0, 0x8204EAF0);           // T and N unpacked: times K11 plus K10
constexpr uint32_t kK10 = BO2_GAME_ADDR(0x8205A7B0, 0x8204EB00);
constexpr uint32_t kOne = BO2_GAME_ADDR(0x8205A2C0, 0x8204E610);           // the last weight: this minus the others
constexpr uint32_t kWeightFill = BO2_GAME_ADDR(0x8205A2A0, 0x8204E5F0);    // the weights' vperm: second input and control
constexpr uint32_t kWeightPerm = BO2_GAME_ADDR(0x84BF3540, 0x844163A0);

inline uint8_t* Ptr(uint8_t* base, uint32_t guest) {
  return rex::memory::GuestPtr<uint8_t*>(base, guest);
}

inline uint32_t Load32(uint8_t* base, uint32_t guest) {
  uint32_t v;
  std::memcpy(&v, Ptr(base, guest), 4);
  return __builtin_bswap32(v);
}

inline uint32_t Load16(uint8_t* base, uint32_t guest) {
  uint16_t v;
  std::memcpy(&v, Ptr(base, guest), 2);
  return __builtin_bswap16(v);
}

// lvx/stvx: 16 bytes at guest & ~15, in memory order (PowerPC byte order).
inline B16 Raw(uint8_t* base, uint32_t guest) {
  return simde_vld1q_u8(Ptr(base, guest & ~15u));
}
inline void Store(uint8_t* base, uint32_t guest, B16 bytes) {
  simde_vst1q_u8(Ptr(base, guest & ~15u), bytes);
}
// Bytes in memory order <-> four floats, lane i = PowerPC element i.
inline F4 Words(B16 bytes) {
  return simde_vreinterpretq_f32_u8(simde_vrev32q_u8(bytes));
}
inline B16 Bytes(F4 v) {
  return simde_vrev32q_u8(simde_vreinterpretq_u8_f32(v));
}
inline F4 Lvx(uint8_t* base, uint32_t guest) {
  return Words(Raw(base, guest));
}

// lvlx: the bytes from guest to the end of its 16-byte block, then zeros.
inline B16 Lvlx(uint8_t* base, uint32_t guest) {
  static const uint8_t kIota[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const B16 index = simde_vaddq_u8(simde_vld1q_u8(kIota), simde_vdupq_n_u8(uint8_t(guest & 15)));
  return simde_vqtbl1q_u8(Raw(base, guest), index);  // indices past 15 give 0
}

// vperm: byte i of the result is byte (control[i] & 31) of a followed by b.
inline B16 Perm(B16 a, B16 b, B16 control) {
  simde_uint8x16x2_t ab;
  ab.val[0] = a;
  ab.val[1] = b;
  return simde_vqtbl2q_u8(ab, simde_vandq_u8(control, simde_vdupq_n_u8(31)));
}

// vmaddfp a * b + c (see the top of the file). Only GCC (the Switch) fuses the recompiled add(mul()): clang builds the
// generated code with -ffp-model=strict (macOS, Linux arm64), which keeps the multiply and the add apart, as on the PC.
inline F4 Madd(F4 a, F4 b, F4 c) {
#if defined(__aarch64__) && !defined(__clang__)
  return simde_vfmaq_f32(c, a, b);
#else
  return simde_vaddq_f32(simde_vmulq_f32(a, b), c);
#endif
}
inline F4 Mul(F4 a, F4 b) {
  return simde_vmulq_f32(a, b);
}
template <int kLane>
inline F4 Splat(F4 v) {
  return simde_vdupq_laneq_f32(v, kLane);
}

// vupkd3d128 type 8, as the recompiled code does it: floats whose bits are 3.0f plus each signed 10-bit field (the
// x field's -512 is a NaN) and 1.0f plus the 2-bit w.
inline F4 Unpack(uint32_t x) {
  const int32_t a = int32_t(x << 22) >> 22;
  const int32_t b = int32_t(x << 12) >> 22;
  const int32_t c = int32_t(x << 2) >> 22;
  const uint32_t w[4] = {a == -512 ? 0x7FC00000u : uint32_t(a + 0x40400000), uint32_t(b + 0x40400000),
                         uint32_t(c + 0x40400000), (x >> 30) | 0x3F800000u};
  return simde_vreinterpretq_f32_u32(simde_vld1q_u32(w));
}

// vpkd3d128 type 2, as the recompiled code does it: each float clamped (v < lo ? lo : v > hi ? hi : v, so a NaN
// stays) to the range whose bits carry its field, then the low 10 bits of x, y and z and the low 2 of w in one word.
struct PackRanges {
  F4 lo, hi;
  U4 mask, shift;
};
inline uint32_t Pack(F4 v, const PackRanges& r) {
  F4 t = simde_vbslq_f32(simde_vcgtq_f32(v, r.hi), r.hi, v);
  t = simde_vbslq_f32(simde_vcltq_f32(v, r.lo), r.lo, t);
  const U4 bits = simde_vshlq_u32(simde_vandq_u32(simde_vreinterpretq_u32_f32(t), r.mask),
                                  simde_vreinterpretq_s32_u32(r.shift));
  return simde_vaddvq_u32(bits);  // the fields do not overlap
}

// vcuxwfp128 ..., 0 as the SDK does it (simde_mm_cvtepu32_ps_ in rex/ppc/intrinsics.h), lane by lane.
inline F4 FromUnsigned(U4 src) {
  const U4 x1 = simde_vaddq_u32(src, simde_vdupq_n_u32(127));
  U4 x0 = simde_vshrq_n_u32(simde_vshlq_n_u32(src, 23), 31);
  x0 = simde_vaddq_u32(x0, x1);
  x0 = simde_vreinterpretq_u32_s32(simde_vshrq_n_s32(simde_vreinterpretq_s32_u32(x0), 8));
  x0 = simde_vaddq_u32(x0, simde_vdupq_n_u32(0x4F800000));
  const F4 x2 = simde_vcvtq_f32_s32(simde_vreinterpretq_s32_u32(src));
  const U4 high = simde_vreinterpretq_u32_s32(simde_vshrq_n_s32(simde_vreinterpretq_s32_u32(src), 31));
  return simde_vbslq_f32(high, simde_vreinterpretq_f32_u32(x0), x2);
}

// The fast form's unpack (vupkd3d128 type 8 above, in vector registers): each field shifted to the top of its lane
// and back down signed, the 2-bit w kept unsigned, the float bits added, and the x field's -512 made the NaN.
struct UnpackFast {
  S4 left;    // {22, 12, 2, 0}
  S4 right;   // {-22, -22, -22, -30}
  U4 keep;    // {~0, ~0, ~0, 3}
  U4 add;     // 3.0f's bits in x, y and z, 1.0f's in w
  S4 nan_at;  // -512 in x, y and z; w never matches
  U4 nan;     // 0x7FC00000
};

struct Constants {
  F4 scale, bias, k8, k9, k10, k11, one, inv_65536;
  B16 perm_position, weight_fill, weight_perm;
  PackRanges pack;
  // The fast form: the weights' vperm puts each weight's two bytes in the low half of a word or makes the word zero
  // (as observed on the console: 10100001 in x, y and z, zeros in w), weight_lanes having all bits set in the first
  // kind; and the position's vperm takes x, y and z from the transformed position and w from the original.
  bool weight_simple = false;
  bool position_simple = false;
  U4 weight_lanes;
  UnpackFast unpack;
};

Constants Read(const PPCContext& ctx, uint8_t* base) {
  Constants k;
  k.scale = Lvx(base, ctx.r8.u32);
  k.bias = Lvx(base, ctx.r8.u32 + 16);
  k.k8 = Lvx(base, kK8);
  k.k9 = Lvx(base, kK9);
  k.k10 = Lvx(base, kK10);
  k.k11 = Lvx(base, kK11);
  k.one = Lvx(base, kOne);
  k.inv_65536 = simde_vreinterpretq_f32_u32(simde_vdupq_n_u32(0x37800000));  // 2^-16
  k.perm_position = Raw(base, kPermPosition);
  k.weight_fill = Raw(base, kWeightFill);
  k.weight_perm = Raw(base, kWeightPerm);
  const float lo[4] = {kPack2101010_Min10, kPack2101010_Min10, kPack2101010_Min10, kPack2101010_Min2};
  const float hi[4] = {kPack2101010_Max10, kPack2101010_Max10, kPack2101010_Max10, kPack2101010_Max2};
  const uint32_t mask[4] = {0x3FF, 0x3FF, 0x3FF, 0x3};
  const uint32_t shift[4] = {0, 10, 20, 30};
  k.pack = {simde_vld1q_f32(lo), simde_vld1q_f32(hi), simde_vld1q_u32(mask), simde_vld1q_u32(shift)};

  uint8_t fill[16], control[16];
  simde_vst1q_u8(fill, k.weight_fill);
  simde_vst1q_u8(control, k.weight_perm);
  static std::atomic<bool> said{false};
  if (!said.load(std::memory_order_relaxed) && !said.exchange(true)) {
    uint8_t position[16];
    simde_vst1q_u8(position, k.perm_position);
    const auto hex = [](const uint8_t* b) {
      std::string s;
      for (int i = 0; i < 16; ++i) {
        s += fmt::format("{}{:02X}", i && (i & 3) == 0 ? " " : "", b[i]);
      }
      return s;
    };
    REXLOG_INFO("[native_skin] the kernels' run-time vectors: weight fill {}, weight vperm {}, position vperm {}",
                hex(fill), hex(control), hex(position));
  }
  // A byte the vperm takes from the fill (indices 16-31) that is zero.
  const auto zero = [&](uint8_t c) { return (c & 31) >= 16 && fill[(c & 31) - 16] == 0; };
  k.weight_simple = true;
  uint32_t lanes[4];
  for (int word = 0; word < 4; ++word) {
    const uint8_t* c = control + 4 * word;
    const bool weight = zero(c[0]) && zero(c[1]) && (c[2] & 31) == 0 && (c[3] & 31) == 1;
    k.weight_simple &= weight || (zero(c[0]) && zero(c[1]) && zero(c[2]) && zero(c[3]));
    lanes[word] = weight ? ~0u : 0u;
  }
  k.weight_lanes = simde_vld1q_u32(lanes);
  simde_vst1q_u8(control, k.perm_position);
  k.position_simple = (control[12] & 31) >= 16 && (control[12] & 3) == 0;
  for (int i = 0; i < 16; ++i) {
    k.position_simple &= i < 12 ? (control[i] & 31) == i : (control[i] & 31) == (control[12] & 31) + (i - 12);
  }

  const int32_t left[4] = {22, 12, 2, 0}, right[4] = {-22, -22, -22, -30}, nan_at[4] = {-512, -512, -512, INT32_MAX};
  const uint32_t keep[4] = {~0u, ~0u, ~0u, 3}, add[4] = {0x40400000, 0x40400000, 0x40400000, 0x3F800000};
  k.unpack = {simde_vld1q_s32(left),   simde_vld1q_s32(right),   simde_vld1q_u32(keep),
              simde_vld1q_u32(add),    simde_vld1q_s32(nan_at), simde_vdupq_n_u32(0x7FC00000)};
  return k;
}

// The fast form's unpack of word kLane of v (lane i = PowerPC word i).
template <int kLane>
inline F4 UnpackLane(U4 v, const UnpackFast& u) {
  const U4 x = simde_vdupq_laneq_u32(v, kLane);
  const S4 fields = simde_vshlq_s32(simde_vreinterpretq_s32_u32(simde_vshlq_u32(x, u.left)), u.right);
  const U4 bits = simde_vaddq_u32(simde_vandq_u32(simde_vreinterpretq_u32_s32(fields), u.keep), u.add);
  return simde_vreinterpretq_f32_u32(simde_vbslq_u32(simde_vceqq_s32(fields, u.nan_at), u.nan, bits));
}

// The fast form's pack: Pack's fields of T and N, summed by pairs: {T's word, N's word}.
inline U4 PackBits(F4 v, const PackRanges& r) {
  F4 t = simde_vbslq_f32(simde_vcgtq_f32(v, r.hi), r.hi, v);
  t = simde_vbslq_f32(simde_vcltq_f32(v, r.lo), r.lo, t);
  return simde_vshlq_u32(simde_vandq_u32(simde_vreinterpretq_u32_f32(t), r.mask),
                         simde_vreinterpretq_s32_u32(r.shift));
}
inline simde_uint32x2_t PackTwo(F4 t, F4 n, const PackRanges& r) {
  const U4 sums = simde_vpaddq_u32(PackBits(t, r), PackBits(n, r));
  return simde_vget_low_u32(simde_vpaddq_u32(sums, sums));
}

// A weight: lvlx at its halfword, the vperm with the fill and the control, vcuxwfp128 ..., 16.
inline F4 Weight(uint8_t* base, uint32_t guest, const Constants& k) {
  const B16 bytes = Perm(Lvlx(base, guest), k.weight_fill, k.weight_perm);
  return Mul(FromUnsigned(simde_vreinterpretq_u32_f32(Words(bytes))), k.inv_65536);
}
// The fast form (weight_simple, and the halfword inside its 16-byte block: an even address): each word is the
// halfword, which converts exactly (and times 2^-16 is exact too), or zero, which converts to +0.
inline F4 WeightFast(uint8_t* base, uint32_t guest, const Constants& k) {
  const F4 w = simde_vdupq_n_f32(float(Load16(base, guest)) * 0x1p-16f);
  return simde_vreinterpretq_f32_u32(simde_vandq_u32(simde_vreinterpretq_u32_f32(w), k.weight_lanes));
}

struct Matrix {
  F4 r0, r1, r2, r3;
};

inline Matrix Bone(uint8_t* base, uint32_t at) {
  return {Lvx(base, at), Lvx(base, at + 16), Lvx(base, at + 32), Lvx(base, at + 48)};
}

// T or N rotated: the recompiled code makes z * row 2 with vmulfp and adds y * row 1 with vmaddfp; kRoundY says
// that GCC rounded y * row 1 and fused z * row 2 (otherwise the other way round). Then x * row 0, fused.
template <bool kRoundY>
inline F4 Chain(F4 v, const Matrix& m) {
  const F4 a = kRoundY ? Madd(Splat<2>(v), m.r2, Mul(Splat<1>(v), m.r1))
                       : Madd(Splat<1>(v), m.r1, Mul(Splat<2>(v), m.r2));
  return Madd(Splat<0>(v), m.r0, a);
}

// One vertex once its matrix is known. kRoundYn and kRoundYt: Chain for N and for T.
template <bool kRoundYn, bool kRoundYt>
inline void Vertex(uint8_t* base, const Constants& k, uint32_t in, uint32_t out, const Matrix& m) {
  // Position: the four 16-bit integers of the vertex's first 8 bytes.
  const simde_int16x4_t h = simde_vreinterpret_s16_u8(simde_vrev16_u8(simde_vld1_u8(Ptr(base, in))));
  const F4 p = Madd(simde_vcvtq_f32_s32(simde_vmovl_s16(h)), k.scale, k.bias);
  // Bytes 8-23 (lvlx | lvrx): two words kept as they are, then T and N.
  const F4 w = Words(simde_vld1q_u8(Ptr(base, in + 8)));
  const F4 t = Madd(Unpack(Load32(base, in + 16)), k.k11, k.k10);
  const F4 n = Madd(Unpack(Load32(base, in + 20)), k.k11, k.k10);

  F4 pa = Madd(m.r2, Splat<2>(p), m.r3);
  pa = Madd(Splat<1>(p), m.r1, pa);
  const F4 pt = Madd(Splat<0>(p), m.r0, pa);
  const F4 nt = Chain<kRoundYn>(n, m);
  const F4 tt = Chain<kRoundYt>(t, m);

  U4 wu = simde_vreinterpretq_u32_f32(w);
  wu = simde_vsetq_lane_u32(Pack(Madd(tt, k.k8, k.k9), k.pack), wu, 2);
  wu = simde_vsetq_lane_u32(Pack(Madd(nt, k.k8, k.k9), k.pack), wu, 3);
  Store(base, out, Perm(Bytes(pt), Bytes(Splat<3>(p)), k.perm_position));
  Store(base, out + 16, Bytes(simde_vreinterpretq_f32_u32(wu)));
}

// Vertex in the fast form (position_simple): the same operations on the same values.
template <bool kRoundYn, bool kRoundYt>
inline void VertexFast(uint8_t* base, const Constants& k, uint32_t in, uint32_t out, const Matrix& m) {
  const simde_int16x4_t h = simde_vreinterpret_s16_u8(simde_vrev16_u8(simde_vld1_u8(Ptr(base, in))));
  const F4 p = Madd(simde_vcvtq_f32_s32(simde_vmovl_s16(h)), k.scale, k.bias);
  // Bytes 8-23: words 2 and 3 are T and N.
  const U4 wu = simde_vreinterpretq_u32_f32(Words(simde_vld1q_u8(Ptr(base, in + 8))));
  const F4 t = Madd(UnpackLane<2>(wu, k.unpack), k.k11, k.k10);
  const F4 n = Madd(UnpackLane<3>(wu, k.unpack), k.k11, k.k10);

  F4 pa = Madd(m.r2, Splat<2>(p), m.r3);
  pa = Madd(Splat<1>(p), m.r1, pa);
  const F4 pt = Madd(Splat<0>(p), m.r0, pa);
  const F4 nt = Chain<kRoundYn>(n, m);
  const F4 tt = Chain<kRoundYt>(t, m);

  const U4 packed =
      simde_vcombine_u32(simde_vget_low_u32(wu), PackTwo(Madd(tt, k.k8, k.k9), Madd(nt, k.k8, k.k9), k.pack));
  Store(base, out, Bytes(simde_vcopyq_laneq_f32(pt, 3, p, 3)));
  Store(base, out + 16, Bytes(simde_vreinterpretq_f32_u32(packed)));
}

// The loop: the matrix of each vertex (MakeMatrix(record index, cached matrix) refills it when the record differs
// from the previous one), then the vertices two by two.
template <bool kRoundYn, bool kRoundYt, bool kFast, typename MakeMatrix>
inline void Run(uint8_t* base, const Constants& k, uint32_t in, uint32_t out, uint32_t count, MakeMatrix make) {
  const auto vertex = [&](uint32_t i, const Matrix& m) {
    if constexpr (kFast) {
      VertexFast<kRoundYn, kRoundYt>(base, k, in + 24 * i, out + 32 * i, m);
    } else {
      Vertex<kRoundYn, kRoundYt>(base, k, in + 24 * i, out + 32 * i, m);
    }
  };
  Matrix m0{}, m1{};
  uint32_t i = 0;
  for (; i + 1 < count; i += 2) {
    make(i, m0);
    m1 = m0;
    make(i + 1, m1);
    vertex(i, m0);
    vertex(i + 1, m1);
    m0 = m1;
  }
  if (i < count) {
    make(i, m0);
    vertex(i, m0);
  }
}

// The fast form in this call: bo2zm_native_skin_fast, or its factor of the A/B arms (bo2zm_native_skin_fast_ab).
bool FastWanted() {
  static const int factor = REXCVAR_GET(bo2zm_native_skin_fast_ab);
  if (factor != 0) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  return REXCVAR_GET(bo2zm_native_skin_fast);
}

// Time per vertex, per kernel and form (1 call in 8 timed), for the report.
struct Timing {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> timed{0};
  std::atomic<uint64_t> vertices{0};
  std::atomic<uint64_t> ns{0};
};
Timing g_timing[3][2];  // [kernel][0 general, 1 fast]

// The thread's own CPU time in ns (on the Switch the kernel's count, svcGetInfo InfoType_ThreadTickCount = 25 at
// 19.2 MHz: time the thread spends preempted is not counted, unlike wall-clock time, which swung timings by 25 %).
#if defined(__SWITCH__)
extern "C" uint32_t svcGetInfo(uint64_t* out, uint32_t id0, uint32_t handle, uint64_t id1);
#endif
uint64_t CpuNs() {
#if defined(__SWITCH__)
  uint64_t ticks = 0;
  svcGetInfo(&ticks, 25, 0xFFFF8000u, UINT64_MAX);
  return ticks * 625 / 12;
#else
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
#endif
}

// Runs body(fast) and times 1 call in 8.
template <typename Body>
void Timed(int kernel, uint32_t count, bool fast, Body body) {
  Timing& t = g_timing[kernel][fast];
  if ((t.calls.fetch_add(1, std::memory_order_relaxed) & 7) != 0) {
    body();
    return;
  }
  const uint64_t t0 = CpuNs();
  body();
  t.ns.fetch_add(CpuNs() - t0, std::memory_order_relaxed);
  t.vertices.fetch_add(count, std::memory_order_relaxed);
  t.timed.fetch_add(1, std::memory_order_relaxed);
}

// Remembers the last bone record (its bytes) so that a vertex with the same one keeps the matrix.
template <int kBytes>
struct LastRecord {
  uint8_t bytes[kBytes];
  bool valid = false;
  bool Same(const uint8_t* record) {
    if (valid && std::memcmp(bytes, record, kBytes) == 0) {
      return true;
    }
    std::memcpy(bytes, record, kBytes);
    valid = true;
    return false;
  }
};

// The kernels' loops, in the general form or the fast one. Each reads its vertices, records and matrices from ctx.
template <bool kFast>
void Run1(const PPCContext& ctx, uint8_t* base, const Constants& k) {
  const uint32_t records = ctx.r4.u32, bones = ctx.r6.u32;
  LastRecord<2> last;
  Run<true, true, kFast>(base, k, ctx.r3.u32, ctx.r7.u32, ctx.r5.u32, [&](uint32_t i, Matrix& m) {
    const uint8_t* record = Ptr(base, records + 2 * i);
    if (!last.Same(record)) {
      m = Bone(base, bones + Load16(base, records + 2 * i));
    }
  });
}

template <bool kFast>
void Run2(const PPCContext& ctx, uint8_t* base, const Constants& k) {
  const uint32_t records = ctx.r4.u32, bones = ctx.r6.u32;
  LastRecord<6> last;
  Run<true, false, kFast>(base, k, ctx.r3.u32, ctx.r7.u32, ctx.r5.u32, [&](uint32_t i, Matrix& m) {
    const uint32_t r = records + 6 * i;
    if (last.Same(Ptr(base, r))) {
      return;
    }
    const Matrix a = Bone(base, bones + Load16(base, r));
    const Matrix b = Bone(base, bones + Load16(base, r + 2));
    const F4 wb = kFast ? WeightFast(base, r + 4, k) : Weight(base, r + 4, k);
    const F4 wa = simde_vsubq_f32(k.one, wb);
    m = {Madd(b.r0, wb, Mul(wa, a.r0)), Madd(b.r1, wb, Mul(wa, a.r1)), Madd(b.r2, wb, Mul(wa, a.r2)),
         Madd(b.r3, wb, Mul(wa, a.r3))};
  });
}

template <bool kFast>
void Run3(const PPCContext& ctx, uint8_t* base, const Constants& k) {
  const uint32_t records = ctx.r4.u32, bones = ctx.r6.u32;
  LastRecord<10> last;
  Run<true, true, kFast>(base, k, ctx.r3.u32, ctx.r7.u32, ctx.r5.u32, [&](uint32_t i, Matrix& m) {
    const uint32_t r = records + 10 * i;
    if (last.Same(Ptr(base, r))) {
      return;
    }
    const Matrix c = Bone(base, bones + Load16(base, r));
    const Matrix a = Bone(base, bones + Load16(base, r + 2));
    const Matrix b = Bone(base, bones + Load16(base, r + 6));
    const F4 wa = kFast ? WeightFast(base, r + 4, k) : Weight(base, r + 4, k);
    const F4 wb = kFast ? WeightFast(base, r + 8, k) : Weight(base, r + 8, k);
    const F4 wc = simde_vsubq_f32(simde_vsubq_f32(k.one, wb), wa);
    m = {Madd(wc, c.r0, Madd(wa, a.r0, Mul(b.r0, wb))), Madd(wc, c.r1, Madd(wa, a.r1, Mul(b.r1, wb))),
         Madd(wc, c.r2, Madd(wa, a.r2, Mul(b.r2, wb))), Madd(wc, c.r3, Madd(wa, a.r3, Mul(b.r3, wb)))};
  });
}

// A kernel: the game's own function for a vertex array that is not 8-byte aligned, nothing for no vertices, else
// RunK in the fast form when it may run (weights: also an even record array, so that no weight crosses its block).
template <void (*kFallback)(PPCContext&, uint8_t*), int kKernel, bool kWeights, typename General, typename FastForm>
void Kernel(PPCContext& ctx, uint8_t* base, General general, FastForm fast_form) {
  if (ctx.r3.u32 & 7) {
    kFallback(ctx, base);
    return;
  }
  const uint32_t count = ctx.r5.u32;
  if (count == 0) {
    return;
  }
  const Constants k = Read(ctx, base);
  ctx.fpscr.enableFlushMode();
  const bool fast = k.position_simple && (!kWeights || (k.weight_simple && (ctx.r4.u32 & 1) == 0)) && FastWanted();
  Timed(kKernel, count, fast, [&] {
    if (fast) {
      fast_form(ctx, base, k);
    } else {
      general(ctx, base, k);
    }
  });
}

}  // namespace

// sub_82912D78. Its fmul: y * row 1 in both chains.
void Skin1(PPCContext& ctx, uint8_t* base) {
  Kernel<__imp__sub_82912D78, 0, false>(ctx, base, Run1<false>, Run1<true>);
}

// sub_82913378. Record: bone A, bone B, B's weight. Its fmul: y * row 1 for N, z * row 2 for T, and (1 - w) * A in
// the blend (w * B fused).
void Skin2(PPCContext& ctx, uint8_t* base) {
  Kernel<__imp__sub_82913378, 1, true>(ctx, base, Run2<false>, Run2<true>);
}

// sub_82913B50. Record: bone C, bone A, A's weight, bone B, B's weight; C takes what the two weights leave. Its fmul:
// y * row 1 in both chains, and wb * B in the blend (wa * A fused, then wc * C).
void Skin3(PPCContext& ctx, uint8_t* base) {
  Kernel<__imp__sub_82913B50, 2, true>(ctx, base, Run3<false>, Run3<true>);
}

// GPU skinning (bo2zm_gpu_skin.h): the call as a job. Only what the fast form also takes: an 8-byte aligned vertex
// array, the game's usual vperm controls and an even record array (the shader reads each weight as the 16-bit
// integer it is and puts the position's w in by a lane move).
bool RecordGpu(int kernel, PPCContext& ctx, uint8_t* base, bool cpu_written) {
  const uint32_t count = ctx.r5.u32;
  uint32_t out = 0;
  if ((ctx.r3.u32 & 7) || count == 0 || !gpu_skin::Physical(ctx.r7.u32 & ~15u, out)) {
    gpu_skin::CountNotRecorded();
    return false;
  }
  const Constants k = Read(ctx, base);
  if (!k.position_simple || (kernel != 0 && (!k.weight_simple || (ctx.r4.u32 & 1)))) {
    gpu_skin::CountNotRecorded();
    return false;
  }
  // The bones the records name (big-endian u16 byte offsets from r6). A record: the bone; or bone A, bone B and B's
  // weight; or bone C, bone A, A's weight, bone B and B's weight. The snapshot spans the lowest to the highest.
  static constexpr uint32_t kHalves[3] = {1, 3, 5};
  const uint32_t halves = kHalves[kernel];
  const uint8_t* records = Ptr(base, ctx.r4.u32);
  uint32_t lo = 0xFFFF, hi = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = records + size_t(i) * halves * 2;
    for (uint32_t h = 0; h < halves; ++h) {
      if (h == 2 || h == 4) {
        continue;  // a weight
      }
      const uint32_t offset = uint32_t(r[2 * h]) << 8 | r[2 * h + 1];
      lo = std::min(lo, offset);
      hi = std::max(hi, offset);
    }
  }
  gpu_skin::JobRef job = gpu_skin::JobRef::New();
  job->out = out;
  job->count = count;
  job->kernel = uint32_t(kernel);
  job->vertices = Ptr(base, ctx.r3.u32);
  job->vertices_guest = ctx.r3.u32;
  job->records = records;
  job->target = Ptr(base, ctx.r7.u32 & ~15u);
  job->records_guest = ctx.r4.u32;
  job->bones_guest = ctx.r6.u32;
  // Each row as lvx reads it: the matrix's address rounded down to 16.
  job->bones_lo = (ctx.r6.u32 + lo) & ~15u;
  const uint32_t bones_hi = ((ctx.r6.u32 + hi) & ~15u) + 64;
  job->bones.resize(bones_hi - job->bones_lo);
  std::memcpy(job->bones.data(), Ptr(base, job->bones_lo), job->bones.size());
  const F4 vectors[7] = {k.scale, k.bias, k.k8, k.k9, k.k10, k.k11, k.one};
  for (int v = 0; v < 7; ++v) {
    simde_vst1q_u32(job->constants + 4 * v, simde_vreinterpretq_u32_f32(vectors[v]));
  }
  simde_vst1q_u32(job->constants + 28, k.weight_lanes);
  job->cpu_written = cpu_written;
  gpu_skin::Add(std::move(job));
  return true;
}

void Report() {
  static const char* const kNames[3] = {"one bone", "two bones", "three bones"};
  static uint64_t said[3][2][3] = {};  // timed, vertices, ns already reported
  std::string line;
  for (int kernel = 0; kernel < 3; ++kernel) {
    line += kernel ? "; " : "";
    line += kNames[kernel];
    for (int fast = 0; fast < 2; ++fast) {
      Timing& t = g_timing[kernel][fast];
      const uint64_t now[3] = {t.timed.load(std::memory_order_relaxed), t.vertices.load(std::memory_order_relaxed),
                               t.ns.load(std::memory_order_relaxed)};
      const uint64_t timed = now[0] - said[kernel][fast][0], vertices = now[1] - said[kernel][fast][1],
                     ns = now[2] - said[kernel][fast][2];
      for (int i = 0; i < 3; ++i) {
        said[kernel][fast][i] = now[i];
      }
      line += fmt::format(" {} {} calls, {:.0f} vertices a call, {:.1f} ns a vertex", fast ? "| fast" : ":", timed,
                          timed ? double(vertices) / double(timed) : 0.0, vertices ? double(ns) / double(vertices) : 0.0);
    }
  }
  REXLOG_INFO("[native_skin] timed (1 call in 8, the thread's CPU time), last 10 s: {}", line);
}

}  // namespace bo2zm::native_skin
