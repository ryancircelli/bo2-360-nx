// A native version of BO2's sound limiter sub_8258BE08 (TU18 default_mp.xex), written from its PowerPC code
// (the generated code of 0x8258BE08-0x8258C034 and of the functions it calls).
//
// sub_8243ED80 calls it five times per call, with r3 = 256 samples (the loop at 0x8243EDBC, r28 = 0..5 except 3;
// f1 = 48000.0 from 0x820520E0), and the sd_mix_master job's sub_8243F0C0 calls sub_8243ED80 twice per block
// (0x8243F1E8 and the call after it), so it runs about 1,870 times a second. At round 15 in Town it costs 2.7 % of a
// core on the Sound Mix thread, and the game's double pow it calls another 0.6 % (from a stack-sampled profile on an
// OLED). r3 = n, r5 = parameters P, r6 = state S, r7 = samples X (in place), r8 = one float out, f1 = a rate.
// What it does:
//   - P+0 == 0: returns at once.
//   - X *= P+4: sub_8258AC98 (ceil(n/4) vectors from X & ~15, vmulfp128 with flush to zero).
//   - attack = sub_8258BDB8(P+20, rate), release = sub_8258BDB8(P+24, rate) (Coefficient below);
//     level = sub_82594280(P+12) (20 log10(max(x, 1.5288e-5)): sub_82919100 is the game's log, sub_82918F70, times
//     0.4342944819 at 0x8210E410); slope = 1 / P+16;
//     makeup = sub_82594228(float(slope * level - level)) (pow(10, x / 20) through the game's pow sub_82918340);
//     exponent = slope - 1; inverse = 1 / makeup.
//   - A DC blocker in place: y = (x - x_prev) + 0.995 y_prev (x_prev, y_prev at S+16, S+20).
//   - The block copied to S + 128 + 4 * S+0 (sub_827D3160: XMemCpy-like, dcbzl on the destination lines), then
//     S+0 = (S+0 + n) & 511. The 2176-byte states are 2176 bytes apart (0x8243EDF0): a 512-sample ring.
//   - The sign bit cleared in X from (X + 16) & ~15 on (vandc128 with the four 0x80000000 words at 0x82070DE0): the
//     first four samples keep theirs.
//   - Per sample i (scalar, single precision, no flush to zero): the envelope (S+4)
//       e = float(float(e + max(|x| - e, 0) * attack) - e * release) + 2^-32  (fmadds, fnmsubs, fadds);
//     the gain is recomputed where i & [0x82A7D29C] is 0 (0x1F in the image: every 32nd sample) or where e rose by more
//     than 1.5288e-5 (0x820119E0) since the last sample: makeup below the threshold P+12, else
//     float(pow(e * threshold, exponent)) * inverse; then out = copy[i] * gain, and the least gain is kept (from 1e6).
//   - S+8 = the last gain, [r8] = inverse * the least gain, S+4 = e; then X *= P+8 * 0.5 (sub_8258AC98 again).
//
// The rewrite does the same operations in the same order and precision: the single-precision adds, subtracts,
// multiplies and divides as float operations (the generated code rounds each double result to a float, which gives the
// same bits: double has more than twice float's precision), the fused ones (fmadds, fnmsubs, fmsubs) as a double
// fused multiply-add rounded to a float, as the generated code does, and the vector scales with flush to zero on, as
// there. The DC blocker, the copy, the sign clearing and the per-sample loop go in one pass (each sample's DC output is
// all the later steps need of it): the DC blocker's chain of dependent operations then overlaps the envelope's. This
// file is built with -ffp-contract=off (app/CMakeLists.txt): GCC must not fuse a product into a later add here.
//
// The setup's log10 and pow(10, x) run once a call: those are the game's own (sub_82594280, sub_82594228), called in
// its frame. The gains' pow (at least every 32nd sample above the threshold, every sample while the envelope rises)
// is the host's (bo2zm_native_limiter_pow): the game's sub_82918340 is a CRT-style double pow (frexp, a 1/16 table,
// polynomials for log2 and 2^x, ldexp: inferred accurate to an ulp or two of a double, like the host's), and the gain
// is rounded to a float right after (frsp): the two can give different floats only when the exact result lies within
// a couple of ulps of a double of a float's rounding boundary, about one gain in 10^8. The checked phase accepts such a
// word within 2^-20 relative (counted apart). The game's pow is still called for anything but a positive finite base
// and a finite non-integer exponent of magnitude below 1024 (its special cases and its loop for integer powers).
//
// A block it does not take runs the game's function: a count that is not a multiple of 32 or above 512, samples not
// 16-byte aligned, a stack pointer not 16-byte aligned (the vector scale loads its splat from the stack), a copy
// destination not 128-byte aligned (sub_827D3160's dcbzl would clear bytes around it), or ranges that overlap.
//
// Verification: an offline harness on the PC ran the generated sub_8258BE08 and this rewrite on the same blocks, each
// with its own state (seven parameter sets, from silence to 8x full scale, denormals included): 20,000 blocks of 256
// samples came out identical word for word, both with the host's pow (~250,000 gains) and with the game's.
//
// On an OLED at round 15, A/B against the game's code: 5,066 checked calls were identical (none merely within the
// tolerance); a call takes 10.6 us instead of 15.2 us, and every block is handled here. The host's pow runs only 0-65
// times per 10 s, because the envelope almost never crosses the threshold: the cost is the per-sample chain of
// dependent operations, not pow. Sound Mix drops from 22.3 to 21.2 % of a core; the frame rate change is within the
// noise.

#include "bo2zm_game.h"
#include "bo2zm_native_limiter.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <simde/arm/neon.h>

#include "bo2zm_benchmark.h"

REXCVAR_DEFINE_BOOL(bo2zm_native_limiter, true, "BO2",
                    "The sound limiter sub_8258BE08 (sd_mix_master's five channels) rewritten natively "
                    "(bo2zm_native_limiter.cpp), checked against the game's own first. false = the game's code");
REXCVAR_DEFINE_INT32(bo2zm_native_limiter_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_native_limiter (on = the "
                     "rewrite, off = the game's code). 0 = not part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_limiter_pow, true, "BO2",
                    "The limiter rewrite computes its gains with the host's pow (they are rounded to a float right "
                    "after, as the game rounds its own double pow's). false = the game's pow, bit for bit")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The game's functions the rewrite calls. They have no hook: tools/direct_calls.py skips lines marked "call only".
REX_EXTERN(__imp__sub_8258BE08);
REX_EXTERN(__imp__sub_82594280);  // call only
REX_EXTERN(__imp__sub_82594228);  // call only
REX_EXTERN(__imp__sub_82918340);  // call only

namespace bo2zm::native_limiter {
namespace {

using Function = void (*)(PPCContext&, uint8_t*);
using F4 = simde_float32x4_t;

constexpr Function kLevel = __imp__sub_82594280;   // call only: 20 log10(max(x, 1.5288e-5)), the game's log
constexpr Function kMakeup = __imp__sub_82594228;  // call only: pow(10, x * 0.05), 0 below 1.5288e-5
constexpr Function kPow = __imp__sub_82918340;     // call only: the game's double pow

// The game's constants (the addresses its lis/lfs pairs form; read at each call).
constexpr uint32_t kOne = BO2_GAME_ADDR(0x82000DB0, 0x82000C60);          // 1.0
constexpr uint32_t kHalf = BO2_GAME_ADDR(0x82000DB8, 0x82000C68);         // 0.5
constexpr uint32_t kZero = BO2_GAME_ADDR(0x82000DC4, 0x82000C74);         // 0.0 (the fsel's other value)
constexpr uint32_t kTwo = BO2_GAME_ADDR(0x8200142C, 0x82001318);          // 2.0 (sub_8258BDB8)
constexpr uint32_t kFour = BO2_GAME_ADDR(0x820035E8, 0x82002A64);         // 4.0 (sub_8258BDB8)
constexpr uint32_t kLeastStart = BO2_GAME_ADDR(0x82003CB4, 0x8202236C);   // 1e6
constexpr uint32_t kTimeFloorD = BO2_GAME_ADDR(0x82004618, 0x82023568);   // 1e-6, a double (sub_8258BDB8's compare)
constexpr uint32_t kRise = BO2_GAME_ADDR(0x820119E0, 0x82019544);         // 1.5288e-5
constexpr uint32_t kDcPole = BO2_GAME_ADDR(0x8205B700, 0x8206BCB8);       // 0.995
constexpr uint32_t kTimeFloor = BO2_GAME_ADDR(0x8206926C, 0x8202379C);    // 1e-6 (sub_8258BDB8's replacement value)
constexpr uint32_t kSignMask = BO2_GAME_ADDR(0x82070DE0, 0x8206BC70);     // four words 0x80000000
constexpr uint32_t kTiny = BO2_GAME_ADDR(0x82070E08, 0x8206BC98);         // 2^-32
constexpr uint32_t kEvery = BO2_GAME_ADDR(0x82A7D29C, 0x8284901C);        // the gain's update mask (0x1F in the image)

inline uint8_t* Host(uint8_t* base, uint32_t guest) {
  return rex::memory::GuestPtr<uint8_t*>(base, guest);
}
// A guest range that maps to one contiguous host range (on PC the physical view from 0xE0000000 sits 0x1000 further).
inline bool Contiguous(uint32_t guest, uint32_t bytes) {
  const uint64_t end = uint64_t(guest) + bytes;
  return end <= 0x1'0000'0000ull && !(guest < 0xE0000000u && end > 0xE0000000ull);
}
inline bool Apart(uint32_t a, uint32_t a_bytes, uint32_t b, uint32_t b_bytes) {
  return uint64_t(a) + a_bytes <= b || uint64_t(b) + b_bytes <= a;
}
inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void Save32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline float FromBits(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline uint32_t ToBits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
inline float Load(const uint8_t* p) { return FromBits(Load32(p)); }
inline void Save(uint8_t* p, float f) { Save32(p, ToBits(f)); }
inline double Load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  v = __builtin_bswap64(v);
  double d;
  std::memcpy(&d, &v, 8);
  return d;
}
inline F4 LoadBE(const uint8_t* p) {
  return simde_vreinterpretq_f32_u8(simde_vrev32q_u8(simde_vld1q_u8(p)));
}
inline void StoreBE(uint8_t* p, F4 v) {
  simde_vst1q_u8(p, simde_vrev32q_u8(simde_vreinterpretq_u8_f32(v)));
}
// The flush-to-zero switches order nothing by themselves: memory accesses stay on their side of these, and a value
// passed through Ready is computed before the next switch.
inline void Fence() { std::atomic_signal_fence(std::memory_order_seq_cst); }
inline void Ready(float& v) {
#if defined(__aarch64__)
  asm volatile("" : "+w"(v));
#elif defined(__x86_64__)
  asm volatile("" : "+x"(v));
#endif
}

// sub_8258AC98(r3 = n, r5 = X, f1 = g): the gain stored as a float (stfs) four times and loaded as one vector, then
// X = g * X over ceil(n/4) vectors (vmulfp128 v63,v62,v63), flush to zero on; it leaves it on.
void Scale(PPCContext& ctx, uint8_t* x, uint32_t n, float g) {
  ctx.fpscr.enableFlushMode();
  Fence();
  const F4 gain = simde_vdupq_n_f32(g);
  for (uint32_t k = 0; k < n / 4; ++k) {
    StoreBE(x + 16 * k, simde_vmulq_f32(gain, LoadBE(x + 16 * k)));
  }
  Fence();
}

// A game function returning f1, called as the original does: in its frame (r1 = sp), with its return address.
double GameF1(PPCContext& ctx, uint8_t* base, Function f, uint32_t sp, uint32_t back, double f1) {
  const uint64_t r1 = ctx.r1.u64;
  const uint64_t lr = ctx.lr;
  ctx.r1.u64 = sp;
  ctx.lr = back;
  ctx.f1.f64 = f1;
  f(ctx, base);
  ctx.r1.u64 = r1;
  ctx.lr = lr;
  ctx.fpscr.disableFlushMode();
  return ctx.f1.f64;
}

// sub_8258BDB8(f1 = t, f2 = rate): a smoothing coefficient.
//   t < 1e-6 (a double compare): t = 1e-6 (the float); a = float(t * rate); a = a * a;
//   return (sqrt(float(a * 4 + 1)) - 1) / (a * 2)  (fmadds, fsqrts, fsubs, fdivs)
float Coefficient(uint8_t* base, float time, double rate) {
  double t = time;
  if (t < Load64(Host(base, kTimeFloorD))) {
    t = Load(Host(base, kTimeFloor));
  }
  const float one = Load(Host(base, kOne));
  float a = float(t * rate);
  a = a * a;
  const float root = std::sqrt(float(std::fma(double(a), double(Load(Host(base, kFour))), double(one))));
  const float twice = a * Load(Host(base, kTwo));
  return (root - one) / twice;
}

// The report: blocks done here and given to the game's function, and the gains' pow calls.
struct Counts {
  std::atomic<uint64_t> native{0}, game{0}, host_pow{0}, game_pow{0};
  uint64_t native_said = 0, game_said = 0, host_pow_said = 0, game_pow_said = 0;
};
Counts g_counts;
std::atomic<uint64_t> g_next_report{0};

uint64_t NowNs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void MaybeReport() {
  const uint64_t now = NowNs();
  uint64_t next = g_next_report.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_report.compare_exchange_strong(next, now + 10'000'000'000ull);
    return;
  }
  if (now < next || !g_next_report.compare_exchange_strong(next, now + 10'000'000'000ull)) {
    return;
  }
  Counts& c = g_counts;
  const uint64_t native = c.native.load(std::memory_order_relaxed), game = c.game.load(std::memory_order_relaxed);
  const uint64_t host_pow = c.host_pow.load(std::memory_order_relaxed);
  const uint64_t game_pow = c.game_pow.load(std::memory_order_relaxed);
  REXLOG_INFO("[native_limiter] last 10 s: {} blocks native, {} given to the game's function (a shape it does not "
              "take); gains computed with the host's pow {}, with the game's {}",
              native - c.native_said, game - c.game_said, host_pow - c.host_pow_said, game_pow - c.game_pow_said);
  c.native_said = native;
  c.game_said = game;
  c.host_pow_said = host_pow;
  c.game_pow_said = game_pow;
}

}  // namespace

bool OnInThisArm() {
  static const int factor = REXCVAR_GET(bo2zm_native_limiter_ab);
  if (factor != 0) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  return REXCVAR_GET(bo2zm_native_limiter);
}

void Limit(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64;
  const uint32_t n = ctx.r3.u32, p = ctx.r5.u32, s = ctx.r6.u32, x = ctx.r7.u32, out = ctx.r8.u32;
  const uint32_t r1 = ctx.r1.u32;
  const uint32_t bytes = n * 4;
  auto game = [&] {
    g_counts.game.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_8258BE08(ctx, base);
  };
  if (!Contiguous(p, 28) || !Contiguous(s, 24)) {
    game();
    return;
  }
  uint8_t* const P = Host(base, p);
  if (Load32(P) == 0) {
    return;  // lwz r11,0(r5); beq: nothing written but its frame
  }
  uint8_t* const S = Host(base, s);
  const uint32_t position = Load32(S);
  const uint32_t copy = s + ((position + 32) << 2);
  if (n == 0 || n > 512 || (n & 31) != 0 || (x & 15) != 0 || (r1 & 15) != 0 || (copy & 127) != 0 ||
      !Contiguous(x, bytes) || !Contiguous(copy, bytes) || !Contiguous(out, 4) || !Apart(x, bytes, s, 24) ||
      !Apart(x, bytes, copy, bytes) || !Apart(x, bytes, out, 4) || !Apart(x, bytes, p, 28) ||
      !Apart(copy, bytes, s, 24) || !Apart(copy, bytes, out, 4) || !Apart(copy, bytes, p, 28) ||
      !Apart(out, 4, s, 24) || !Apart(out, 4, p, 28)) {
    game();
    return;
  }
  uint8_t* const X = Host(base, x);
  uint8_t* const C = Host(base, copy);
  const uint32_t sp = r1 - 256;  // its frame (stwu r1,-256(r1)), where its calls run

  // X *= P+4 (bl 0x8258ac98 at 0x8258BE48).
  Scale(ctx, X, n, Load(P + 4));
  ctx.fpscr.disableFlushMode();
  Fence();

  // The setup, 0x8258BE4C-0x8258BEA4. The rate stays in f2 for both coefficients (fmr f2,f1).
  const double rate = ctx.f1.f64;
  const float attack = Coefficient(base, Load(P + 20), rate);   // f23
  const float release = Coefficient(base, Load(P + 24), rate);  // f22
  const float threshold = Load(P + 12);                         // f30
  const double level = GameF1(ctx, base, kLevel, sp, BO2_GAME_ADDR(0x8258BE70, 0x8249BBC0), threshold);
  const float one = Load(Host(base, kOne));
  const float slope = one / Load(P + 16);                                               // fdivs f29,f31,f0
  const double makeup_db = float(std::fma(double(slope), level, -level));               // fmsubs f1,f29,f1,f1
  const float makeup = float(GameF1(ctx, base, kMakeup, sp, BO2_GAME_ADDR(0x8258BE88, 0x8249BBD8), makeup_db));  // f25
  const float exponent = slope - one;                                                   // fsubs f21,f29,f31
  const float inverse = one / makeup;                                                   // fdivs f26,f31,f1

  // The per-sample constants and state.
  const double pole = Load(Host(base, kDcPole));  // f11
  const float tiny = Load(Host(base, kTiny));      // f27
  const float zero = Load(Host(base, kZero));      // f28
  const float rise = Load(Host(base, kRise));      // f24
  uint32_t sign[4];
  for (int k = 0; k < 4; ++k) {
    sign[k] = Load32(Host(base, kSignMask + 4 * k));
  }
  const uint32_t every = Load32(Host(base, kEvery));
  float in_prev = Load(S + 16), dc = Load(S + 20);           // f13, f0 of the DC blocker
  float envelope = Load(S + 4), gain = Load(S + 8);           // f31, f0
  float least = Load(Host(base, kLeastStart));                // f29
  static const bool host_pow_wanted = REXCVAR_GET(bo2zm_native_limiter_pow);
  const bool host_pow = host_pow_wanted && std::isfinite(exponent) && exponent != std::trunc(exponent) &&
                        std::fabs(exponent) < 1024.0f;
  uint32_t host_pows = 0, game_pows = 0;

  for (uint32_t i = 0; i < n; ++i) {
    // The DC blocker (0x8258BEB4): fsubs f10,f12,f13; fmadds f0,f0,f11,f10; stfsu.
    const float in = Load(X + 4 * i);
    const float step = in - in_prev;
    in_prev = in;
    dc = float(std::fma(double(dc), pole, double(step)));
    // The copy (bl 0x827d3160), then the sign cleared from X + 16 on (0x8258BF3C).
    Save(C + 4 * i, dc);
    const float level_in = i < 4 ? dc : FromBits(ToBits(dc) & ~sign[i & 3]);
    // The envelope (0x8258BF80): fsubs f12,f13,f31; fsel f12,f12,f12,f28; fmadds f12,f12,f23,f31;
    // fnmsubs f12,f31,f22,f12; fadds f31,f12,f27.
    const float before = envelope;
    float up = level_in - envelope;
    up = up >= 0.0f ? up : zero;
    float e = float(std::fma(double(up), double(attack), double(envelope)));
    e = float(-std::fma(double(envelope), double(release), -double(e)));
    envelope = e + tiny;
    // The gain (and. r11,r11,r27 / fadds f13,f13,f24; fcmpu; fcmpu f31,f30): makeup, or
    // fmuls f1,f31,f30; bl 0x82918340; frsp f0,f1; fmuls f0,f0,f26.
    if ((every & i) == 0 || envelope > before + rise) {
      if (envelope < threshold) {
        gain = makeup;
      } else {
        const float base_value = envelope * threshold;
        double power;
        if (host_pow && base_value > 0.0f && base_value < INFINITY) {
          power = std::pow(double(base_value), double(exponent));
          ++host_pows;
        } else {
          ctx.f2.f64 = exponent;
          power = GameF1(ctx, base, kPow, sp, BO2_GAME_ADDR(0x8258BFD0, 0x8249BD20), base_value);
          ++game_pows;
        }
        gain = float(power) * inverse;
      }
    }
    // 0x8258BFD8: fsubs f13,f29,f0; lfsx f12 (the copy); fmuls f12,f12,f0; stfs; fsel f29,f13,f0,f29.
    const float drop = least - gain;
    Save(X + 4 * i, dc * gain);
    least = drop >= 0.0f ? gain : least;
  }

  // 0x8258BECC-0x8258BF2C and 0x8258BFFC-0x8258C024.
  Save(S + 16, in_prev);
  Save(S + 20, dc);
  Save32(S, (position + n) & 0x1FF);
  Save(S + 8, gain);
  Save(Host(base, out), inverse * least);  // fmuls f0,f26,f29
  Save(S + 4, envelope);
  float post = Load(P + 8) * Load(Host(base, kHalf));  // fmuls f1,f13,f0
  Ready(post);
  Fence();
  Scale(ctx, X, n, post);  // bl 0x8258ac98 at 0x8258C024: leaves flush to zero on, as there
  ctx.r3.u64 = r3;          // mr r3,r30 before it (the game's calls above may have used r3)

  g_counts.native.fetch_add(1, std::memory_order_relaxed);
  if (host_pows) {
    g_counts.host_pow.fetch_add(host_pows, std::memory_order_relaxed);
  }
  if (game_pows) {
    g_counts.game_pow.fetch_add(game_pows, std::memory_order_relaxed);
  }
  MaybeReport();
}

}  // namespace bo2zm::native_limiter
