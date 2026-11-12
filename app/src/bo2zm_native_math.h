// Hand-written replacements for five small recompiled functions, bit-identical to the generated C++ (the reference):
// the pure part (no hooks, no cvars, no logging), so tools/tests/native_math_test.cpp can compile exactly these
// functions against the generated bodies. The hooks, switches and the checked mode are in bo2zm_native_math.cpp.
//
// Every function takes the register context and the guest memory base, reads and writes what the generated function
// does, and leaves the same value in the registers the callers read:
//   sub_8289B6F0  r3 (0 or 1)
//   sub_8248B820  memory only (9 floats at r4)
//   sub_829170B0  f1 (sin) and the four stack scratch stores below r1
//   sub_82917190  f1 (cos) and the two stack scratch stores below r1
//   sub_829160B8  f1 (floor)
// All five also do what the generated code does to the FPSCR model (ctx.fpscr.disableFlushMode() when they get as far
// as the first float instruction). The other registers the generated code clobbers (f0, f2-f13, r8-r11 as listed per
// function in /tmp/bo2/guest-hot/math/RESULT.md) are NOT written: a scan of every call site in the generated code
// shows none reads them after the call, and the native versions never touch registers the original leaves alone (a
// caller of sub_8248B820 keeps r9 across the call).
//
// The generated code is compiled on the platform's own simde: fctid there is (int64_t)round(x) (half away from zero,
// saturating) on ARM64, fctidz is a truncating cast. The AArch64 path uses the equivalent instructions (fcvtas,
// fcvtzs) and is checked against the generated code by the offline harness; other hosts use the same simde calls the
// generated code does.

#pragma once

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/ppc/context.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#else
#include <rex/ppc/intrinsics.h>
#endif

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace bo2zm::nmath {

// Guest addresses (default_mp.xex, the Zombies program the app builds).
constexpr uint32_t kAddrOutside = 0x82000DC4;   // float, sub_8289B6F0: the plane distance must be above it
constexpr uint32_t kAddrQuatTwo = 0x8200142C;   // float 2.0, sub_8248B820
constexpr uint32_t kAddrQuatOne = 0x82000DB0;   // float 1.0, sub_8248B820
constexpr uint32_t kAddrTrig = 0x82A9CCE0;      // the sin/cos table (doubles from +0, floats at +24..+36)
constexpr uint32_t kAddrOneD = 0x8200A068;      // double 1.0
constexpr uint32_t kAddrNanD = 0x82A9D3E0;      // double, the quiet NaN sin/cos return for huge arguments
constexpr uint32_t kAddrLimit = 0x8210E220;     // double 1e18: sub_829160B8 floors below it

inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
inline uint64_t Ld64(const uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, base + a, 8);
  return __builtin_bswap64(v);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
inline void St64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(base + a, &v, 8);
}
inline float LdF(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(Ld32(base, a)); }
inline double LdD(const uint8_t* base, uint32_t a) { return std::bit_cast<double>(Ld64(base, a)); }

// fctid / fctidz exactly as the generated code computes them (NaN gives INT64_MIN; out of range saturates).
inline int64_t Fctid(double x) {
#if defined(__aarch64__)
  return std::isnan(x) ? INT64_MIN : vcvtad_s64_f64(x);
#else
  return std::isnan(x) ? int64_t(0x8000000000000000ULL)
         : (x > double(LLONG_MAX)) ? LLONG_MAX
                                   : simde_mm_cvtsd_si64(simde_mm_load_sd(&x));
#endif
}
inline int64_t Fctidz(double x) {
#if defined(__aarch64__)
  return std::isnan(x) ? INT64_MIN : vcvtd_s64_f64(x);
#else
  return std::isnan(x) ? int64_t(0x8000000000000000ULL)
         : (x > double(LLONG_MAX)) ? LLONG_MAX
                                   : simde_mm_cvttsd_si64(simde_mm_load_sd(&x));
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// sub_8289B6F0(point r3, planes r5, count r6, f1) -> r3: 1 as soon as a plane's distance is not above the float at
// kAddrOutside, else 0. Planes: 20 bytes, floats a, b, c, d at +0, +4, +8, +12.
//   dist = fadds(fadds(fmadds(c, z, fmadds(a, x, fmuls(b, y))), d), f1)
// The two fmadds are fused in double and rounded to float (double rounding, as the generated code does); the
// products and sums of floats are exact in double, so float arithmetic gives the same bits (and the same "greater"
// result for NaN, the only thing the NaN's payload could change).
// ---------------------------------------------------------------------------------------------------------------
inline void Sub8289B6F0(PPCContext& ctx, uint8_t* base) {
  const int32_t count = ctx.r6.s32;
  if (count <= 0) {
    ctx.r3.s64 = 0;
    return;
  }
  ctx.fpscr.disableFlushMode();
  const uint32_t point = ctx.r3.u32;
  const float xf = LdF(base, point + 0), yf = LdF(base, point + 4), zf = LdF(base, point + 8);
  const double x = xf, z = zf;
  const double bias = ctx.f1.f64;
  const double outside = double(LdF(base, kAddrOutside));
  uint32_t at = ctx.r5.u32;
  for (int32_t i = 0; i < count; ++i, at += 20) {
    const float a = LdF(base, at), b = LdF(base, at + 4), c = LdF(base, at + 8), d = LdF(base, at + 12);
    const float by = b * yf;
    const float t1 = float(std::fma(double(a), x, double(by)));
    const float t2 = float(std::fma(double(c), z, double(t1)));
    const float t3 = t2 + d;
    const float dist = float(double(t3) + bias);
    if (!(double(dist) > outside)) {
      ctx.r3.s64 = 1;
      return;
    }
  }
  ctx.r3.s64 = 0;
}

// FMUL/FADD pinned to a fixed operand order (found by search to equal what clang -O2 emitted for the generated
// control; see RESULT.md, doubt on NaN payload order): with two different NaN payloads the hardware returns the first
// operand, and the compiler is free to commute a plain a * b, which would change which payload survives.
inline float MulF(float a, float b) {
#if defined(__aarch64__)
  float r;
  asm("fmul %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
  return r;
#else
  return a * b;
#endif
}
inline float AddF(float a, float b) {
#if defined(__aarch64__)
  float r;
  asm("fadd %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
  return r;
#else
  return a + b;
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// sub_8248B820(quaternion r3: x y z w, matrix r4): nine floats at r4 + 0..32.
// ---------------------------------------------------------------------------------------------------------------
inline void Sub8248B820(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.disableFlushMode();
  const uint32_t q = ctx.r3.u32;
  const float z = LdF(base, q + 8), y = LdF(base, q + 4), x = LdF(base, q + 0), w = LdF(base, q + 12);
  const float two = LdF(base, kAddrQuatTwo);
  const float one = LdF(base, kAddrQuatOne);
  const float x2 = MulF(x, two);
  const float y2 = MulF(y, two);
  const float z2 = MulF(z, two);
  const float xx = MulF(x, x2);
  const float yy = MulF(y, y2);
  const float zz = MulF(z, z2);
  const float yx = MulF(x2, y);
  const float wz = MulF(z2, w);
  const float zx = MulF(x2, z);
  const float wy = MulF(w, y2);
  const float wx = MulF(x2, w);
  const float zy = MulF(z, y2);
  const float s_yy_xx = AddF(yy, xx);
  const float s_zz_yy = AddF(zz, yy);
  const float s_zz_xx = AddF(zz, xx);
  const uint32_t m = ctx.r4.u32;
  St32(base, m + 4, std::bit_cast<uint32_t>(AddF(wz, yx)));
  St32(base, m + 12, std::bit_cast<uint32_t>(yx - wz));
  St32(base, m + 8, std::bit_cast<uint32_t>(zx - wy));
  St32(base, m + 24, std::bit_cast<uint32_t>(AddF(wy, zx)));
  St32(base, m + 20, std::bit_cast<uint32_t>(AddF(zy, wx)));
  St32(base, m + 28, std::bit_cast<uint32_t>(zy - wx));
  St32(base, m + 32, std::bit_cast<uint32_t>(one - s_yy_xx));
  St32(base, m + 0, std::bit_cast<uint32_t>(one - s_zz_yy));
  St32(base, m + 16, std::bit_cast<uint32_t>(one - s_zz_xx));
}

// ---------------------------------------------------------------------------------------------------------------
// sin (sub_829170B0) and cos (sub_82917190): double polynomial after a reduction by pi, tables read once from
// guest memory (the image's read-only data; the checked mode re-reads them live on every check).
// ---------------------------------------------------------------------------------------------------------------
struct Tables {
  double half_pi, inv_pi, c16, c40, c48;       // table +0, +8, +16, +40, +48
  double p[8];                                 // +56 .. +112: the polynomial, lowest order first
  double one, nan, limit;                      // 0x8200A068, 0x82A9D3E0, 0x8210E220
  double f_pos1, f_neg1, f_half, f_zero;       // the floats at +28, +32, +36, +24 as doubles
};

inline Tables LoadTables(const uint8_t* base) {
  Tables t{};
  t.half_pi = LdD(base, kAddrTrig + 0);
  t.inv_pi = LdD(base, kAddrTrig + 8);
  t.c16 = LdD(base, kAddrTrig + 16);
  t.c40 = LdD(base, kAddrTrig + 40);
  t.c48 = LdD(base, kAddrTrig + 48);
  for (int i = 0; i < 8; ++i) {
    t.p[i] = LdD(base, kAddrTrig + 56 + 8 * i);
  }
  t.one = LdD(base, kAddrOneD);
  t.nan = LdD(base, kAddrNanD);
  t.limit = LdD(base, kAddrLimit);
  t.f_pos1 = double(LdF(base, kAddrTrig + 28));
  t.f_neg1 = double(LdF(base, kAddrTrig + 32));
  t.f_half = double(LdF(base, kAddrTrig + 36));
  t.f_zero = double(LdF(base, kAddrTrig + 24));
  return t;
}

// The shared tail: reduced argument r (= f9), the polynomial in r*r, then r * poly.
inline double TrigPoly(const Tables& t, double r) {
  const double z = r * r;
  double v = std::fma(t.p[7], z, t.p[6]);
  v = std::fma(v, z, t.p[5]);
  v = std::fma(v, z, t.p[4]);
  v = std::fma(v, z, t.p[3]);
  v = std::fma(v, z, t.p[2]);
  v = std::fma(v, z, t.p[1]);
  v = std::fma(v, z, t.p[0]);
  v = std::fma(v, z, t.one);
  return v * r;
}

inline uint64_t FlipIf(uint64_t odd, double v_in, double& out) {
  uint64_t b = std::bit_cast<uint64_t>(v_in);
  b ^= (odd & 1) << 63;
  out = std::bit_cast<double>(b);
  return b;
}

inline void Sub829170B0(PPCContext& ctx, uint8_t* base, const Tables& t) {
  ctx.fpscr.disableFlushMode();
  const uint64_t xbits = ctx.f1.u64;
  const uint64_t abits = xbits & ~0x8000000000000000ULL;
  const double a = std::bit_cast<double>(abits);
  const int64_t n = Fctid(t.inv_pi * a);
  const double nd = double(n);
  const double sign = ctx.f1.f64 >= 0.0 ? t.f_pos1 : t.f_neg1;
  const double r1 = -std::fma(t.c40, nd, -a);
  const int64_t nz = Fctidz(nd);
  const double r = -std::fma(t.c48, nd, -r1);
  double v;
  FlipIf(uint64_t(nz), TrigPoly(t, r), v);
  const double res = v * sign;
  const uint32_t sp = ctx.r1.u32;
  St64(base, sp - 16, 0);  // stfd f30 / f31: the recompiled function's own (zero) locals
  St64(base, sp - 8, 0);
  St64(base, sp - 24, abits);
  St64(base, sp - 32, uint64_t(nz));
  if (abits != 0) {
    ctx.f1.f64 = (a - t.c16) >= 0.0 ? t.nan : res;
  }
}

inline void Sub82917190(PPCContext& ctx, uint8_t* base, const Tables& t) {
  ctx.fpscr.disableFlushMode();
  const uint64_t abits = ctx.f1.u64 & ~0x8000000000000000ULL;
  const double a = std::bit_cast<double>(abits);
  const double s = t.half_pi + a;
  const int64_t n = Fctid(t.inv_pi * s);
  const double nd = double(n);
  const double m = nd - t.f_half;
  const int64_t nz = Fctidz(nd);
  const double r1 = -std::fma(t.c40, m, -a);
  const double r = -std::fma(t.c48, m, -r1);
  double v;
  FlipIf(uint64_t(nz), TrigPoly(t, r), v);
  const uint32_t sp = ctx.r1.u32;
  St64(base, sp - 8, 0);  // stfd f31: the recompiled function's own (zero) local
  St64(base, sp - 16, uint64_t(nz));
  if (a == t.f_zero) {
    ctx.f1.f64 = t.f_pos1;
    return;
  }
  ctx.f1.f64 = (s - t.c16) >= 0.0 ? t.nan : v;
}

// ---------------------------------------------------------------------------------------------------------------
// sub_829160B8(f1): floor (truncate, minus 1 when the remainder is negative) for |x| up to 1e18, x itself otherwise (and for NaN and zero).
// ---------------------------------------------------------------------------------------------------------------
inline void Sub829160B8(PPCContext& ctx, const Tables& t) {
  ctx.fpscr.disableFlushMode();
  const double x = ctx.f1.f64;
  const double a = std::bit_cast<double>(ctx.f1.u64 & ~0x8000000000000000ULL);
  const double tr = double(Fctidz(x));
  const double floored = (x - tr) >= 0.0 ? tr : tr - t.one;
  const double in_range = (t.limit - a) >= 0.0 ? floored : x;
  ctx.f1.f64 = (-a) >= 0.0 ? x : in_range;
}

}  // namespace bo2zm::nmath
