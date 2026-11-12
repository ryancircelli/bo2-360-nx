// Offline equivalence test and microbenchmark for app/src/bo2zm_native_math.h.
// The controls are the read-only generated bodies (spliced in by test_native_math.py);
// the natives are the production functions from the header. No game assets are required
// at run time except an optional image path for the constant tables.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <sys/mman.h>

uint8_t* memory;

template <typename T> T Load(uint8_t* base, uint32_t address) {
  T value;
  std::memcpy(&value, base + address, sizeof(value));
  if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
  else if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
  else return __builtin_bswap64(value);
}
template <typename T> void Store(uint8_t* base, uint32_t address, T value) {
  if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
  else value = __builtin_bswap64(value);
  std::memcpy(base + address, &value, sizeof(value));
}

#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_GUEST_BARRIER() ((void)0)
#define REX_LOAD_U16(a) Load<uint16_t>(base, uint32_t(a))
#define REX_LOAD_U32(a) Load<uint32_t>(base, uint32_t(a))
#define REX_LOAD_U64(a) Load<uint64_t>(base, uint32_t(a))
#define REX_STORE_U32(a, v) Store<uint32_t>(base, uint32_t(a), uint32_t(v))
#define REX_STORE_U64(a, v) Store<uint64_t>(base, uint32_t(a), uint64_t(v))
#define DEFINE_REX_FUNC(name) extern "C" REX_FUNC(__imp__##name)

// CONTROL_BODIES

#include "bo2zm_native_math.h"
using namespace bo2zm::nmath;

constexpr size_t kMemSize = 0x82B00000;  // reserved, only touched pages are committed
constexpr uint32_t kStack = 0x00200000;  // r1
constexpr uint32_t kQuatIn = 0x00210000, kQuatOut = 0x00211000, kPlanes = 0x00220000;
constexpr uint32_t kWatchLo = kStack - 64, kWatchLen = 64 + 64;  // stack scratch
constexpr uint32_t kOutLen = 64;

static void Put32(uint32_t a, uint32_t v) { Store<uint32_t>(memory, a, v); }
static void Put64(uint32_t a, uint64_t v) { Store<uint64_t>(memory, a, v); }

static void LoadConstants(const char* image) {
  struct Span { uint32_t addr, len; };
  const Span spans[] = {{0x82000DB0, 4}, {0x82000DC4, 4}, {0x8200142C, 4}, {0x8200A068, 8},
                        {0x82A9CCE0, 128}, {0x82A9D3E0, 8}, {0x8210E220, 8}};
  FILE* f = image ? std::fopen(image, "rb") : nullptr;
  bool ok = f != nullptr;
  if (f) {
    for (auto s : spans) {
      if (std::fseek(f, long(s.addr - 0x82000000), SEEK_SET) != 0 ||
          std::fread(memory + s.addr, 1, s.len, f) != s.len) { ok = false; break; }
    }
    std::fclose(f);
  }
  if (ok) { std::printf("constants: read from game image %s\n", image); return; }
  std::printf("constants: game image unavailable, using values recorded from it\n");
  Put32(0x82000DB0, 0x3F800000); Put32(0x82000DC4, 0); Put32(0x8200142C, 0x40000000);
  Put64(0x8200A068, 0x3FF0000000000000ull); Put64(0x82A9D3E0, 0xFFF8000000000000ull);
  Put64(0x8210E220, 0x43ABC16D674EC800ull);
  const uint64_t tbl[] = {0x3ff921fb54442d18, 0x3fd45f306dc9c883, 0x41aa39de00000000, 0, 0,
                          0x400921fb54400000, 0x3de0b4611a600000, 0xbfc5555555555555, 0x3f811111111110b0,
                          0xbf2a01a01a013e1a, 0x3ec71de3a524f063, 0xbe5ae6454b5dc0ab, 0x3de6123c686ad430,
                          0xbd6ae420dc08499c, 0x3ce880ff6993df95, 0};
  for (int i = 0; i < 16; ++i) Put64(0x82A9CCE0 + 8 * i, tbl[i]);
  Put32(0x82A9CCE0 + 24, 0); Put32(0x82A9CCE0 + 28, 0x3F800000);
  Put32(0x82A9CCE0 + 32, 0xBF800000); Put32(0x82A9CCE0 + 36, 0x3F000000);
}

// ---------------------------------------------------------------- random sources
static std::mt19937_64 rng(0xB02B02);
static uint64_t R() { return rng(); }
static double U01() { return double(R() >> 11) * (1.0 / 9007199254740992.0); }

static const double kPiHalf = 1.5707963267948966;
static double SpecialD() {
  static const double s[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, 1e-300, -1e-300, 4.9e-324, -4.9e-324,
      2.2250738585072014e-308, 1e18, -1e18, 1e18 + 128, 9.3e18, -9.3e18, 1.8e19, 4503599627370496.0,
      2.2e8, -2.2e8, 220000000.00000003, 219999999.99999997, 2.3e8, 1e10, 1e100, 1.7976931348623157e308,
      -1.7976931348623157e308, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(), -std::numeric_limits<double>::quiet_NaN(), 3.141592653589793,
      6.283185307179586, 0.1, 1e-10};
  return s[R() % (sizeof(s) / sizeof(s[0]))];
}
static double RandD(bool trig) {
  switch (R() % (trig ? 11 : 9)) {
    case 0: return SpecialD();
    case 1: return std::bit_cast<double>(R());                       // any bit pattern
    case 2: return (U01() - 0.5) * 20.0;
    case 3: return (U01() - 0.5) * 2000.0;
    case 4: { double k = double(int64_t(R() % 2000001) - 1000000); return k * kPiHalf; }
    case 5: { double k = double(int64_t(R() % 2000001) - 1000000);
              double v = k * kPiHalf; return std::bit_cast<double>(std::bit_cast<int64_t>(v) + int64_t(R() % 7) - 3); }
    case 6: return (U01() - 0.5) * 4.6e8;                            // spans the 2.2e8 limit
    case 7: return double(int64_t(R() % 4001) - 2000) * 0.5;         // integers and halves
    case 8: return std::ldexp(U01() - 0.5, int(R() % 140) - 70);
    case 9: { double k = double(int64_t(R() % 100) - 50); return k * 3.141592653589793 + double(int(R() % 3) - 1) * 1e-9; }
    default: { double k = double(int64_t(R() % 400000000) ); double v = k * kPiHalf; return (R() & 1) ? v : -v; }
  }
}
static float SpecialF() {
  static const float s[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1e-45f, -1e-45f, 1.17549435e-38f, 1e30f, -1e30f,
      3.4028235e38f, -3.4028235e38f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::quiet_NaN(), 16777216.0f, 1e-20f};
  return s[R() % (sizeof(s) / sizeof(s[0]))];
}
static float RandF(float scale = 1.0f) {
  switch (R() % 8) {
    case 0: return SpecialF();
    case 1: return std::bit_cast<float>(uint32_t(R()));
    case 2: return (float(U01()) - 0.5f) * 2.0f * scale;
    case 3: return (float(U01()) - 0.5f) * 200.0f * scale;
    case 4: return std::ldexp(float(U01()) - 0.5f, int(R() % 80) - 40);
    case 5: return float(int(R() % 21) - 10);
    default: return (float(U01()) - 0.5f) * 2.0f;
  }
}
static void PutF(uint32_t a, float v) { Put32(a, std::bit_cast<uint32_t>(v)); }

// ---------------------------------------------------------------- comparison harness
struct Result {
  uint64_t calls = 0, mismatches = 0;
};
static std::vector<uint8_t> Snap(uint32_t lo, uint32_t len) { return {memory + lo, memory + lo + len}; }

static PPCContext Fresh() {
  PPCContext c{};
  c.fpscr.csr = 0;
  return c;
}

static bool SameCtx(const PPCContext& a, const PPCContext& b, bool f1_matters, bool r3_matters) {
  if (r3_matters && a.r3.u64 != b.r3.u64) return false;
  if (f1_matters && a.f1.u64 != b.f1.u64) return false;
  return a.fpscr.csr == b.fpscr.csr;
}
// natives must leave every register the original also leaves alone, untouched.
static bool UntouchedOk(const PPCContext& in, const PPCContext& out) {
  const auto* a = &in.r0; const auto* b = &out.r0;
  (void)a; (void)b;
  return in.r1.u64 == out.r1.u64 && in.r4.u64 == out.r4.u64 && in.r5.u64 == out.r5.u64 && in.r6.u64 == out.r6.u64 &&
         in.r7.u64 == out.r7.u64 && in.r9.u64 == out.r9.u64 && in.r30.u64 == out.r30.u64 && in.r31.u64 == out.r31.u64 &&
         in.lr == out.lr;
}

static void Report(const char* name, const Result& r) {
  std::printf("%-12s calls=%llu mismatches=%llu\n", name, (unsigned long long)r.calls, (unsigned long long)r.mismatches);
}
static int g_fail = 0;
static const char* g_cur = "";
static void Fail(const char* what, const PPCContext& in) {
  if (g_fail++ < 20) std::printf("MISMATCH %s %s f1=%016llx r3=%016llx r4=%llx r5=%llx r6=%llx\n", g_cur, what,
      (unsigned long long)in.f1.u64, (unsigned long long)in.r3.u64, (unsigned long long)in.r4.u64,
      (unsigned long long)in.r5.u64, (unsigned long long)in.r6.u64);
}

constexpr uint64_t kCalls = 1000000;

static Result TestPlane(uint64_t n) {
  Result res; g_cur = "plane";
  for (uint64_t i = 0; i < n; ++i) {
    uint32_t planes = (R() % 6 == 0) ? uint32_t(R() % 12) : 1 + uint32_t(R() % 8);
    PPCContext in = Fresh();
    in.r3.u64 = kQuatIn; in.r5.u64 = kPlanes; in.r6.s64 = int32_t(planes);
    if (R() % 50 == 0) in.r6.s64 = -int32_t(R() % 3);
    if (R() % 50 == 0) in.r6.u64 = 0xFFFFFFFF00000000ull | (R() % 8);  // upper garbage; test uses low 32 as signed
    in.r1.u64 = kStack; in.r4.u64 = 7; in.r7.u64 = 11; in.r9.u64 = 0x1234; in.f1.u64 = R() % 4 ? std::bit_cast<uint64_t>(double(RandF(10))) : std::bit_cast<uint64_t>(RandD(false));
    bool small = R() % 3 == 0;
    for (int k = 0; k < 3; ++k) PutF(kQuatIn + 4 * k, small ? float(int(R() % 5) - 2) : RandF(10));
    uint32_t cnt = std::max<uint32_t>(planes, 1) + 1;
    for (uint32_t p = 0; p < cnt * 1; ++p)
      for (int k = 0; k < 5; ++k) PutF(kPlanes + 20 * p + 4 * k, small ? float(int(R() % 5) - 2) : RandF(5));
    // threshold: usually the real 0.0, sometimes random
    uint32_t th = (R() % 8 == 0) ? std::bit_cast<uint32_t>(RandF()) : 0;
    Put32(0x82000DC4, th);
    PPCContext a = in, b = in;
    __imp__sub_8289B6F0(a, memory);
    Sub8289B6F0(b, memory);
    ++res.calls;
    if (!SameCtx(a, b, false, true) || !UntouchedOk(in, b)) { ++res.mismatches; Fail("regs", in); }
  }
  Put32(0x82000DC4, 0);
  return res;
}

static Result TestQuat(uint64_t n) {
  Result res; g_cur = "quat";
  for (uint64_t i = 0; i < n; ++i) {
    PPCContext in = Fresh();
    bool alias = R() % 8 == 0;
    in.r3.u64 = kQuatIn; in.r4.u64 = alias ? kQuatIn : kQuatOut;
    in.r1.u64 = kStack; in.r5.u64 = 5; in.r9.u64 = 0x4242; in.f1.u64 = 0x1111;
    for (int k = 0; k < 4; ++k) PutF(kQuatIn + 4 * k, (i & 3) ? RandF() : float(int(R() % 3) - 1));
    for (uint32_t k = 0; k < 64; k += 4) Put32(kQuatOut + k, uint32_t(R()));
    auto m0 = Snap(kQuatIn, 0x1000 + kOutLen);
    PPCContext a = in, b = in;
    __imp__sub_8248B820(a, memory);
    auto ma = Snap(kQuatIn, 0x1000 + kOutLen);
    std::copy(m0.begin(), m0.end(), memory + kQuatIn);
    Sub8248B820(b, memory);
    auto mb = Snap(kQuatIn, 0x1000 + kOutLen);
    ++res.calls;
    bool ok = ma == mb && SameCtx(a, b, false, false) && UntouchedOk(in, b);
    if (!ok) { ++res.mismatches; Fail("mem/regs", in); }
  }
  return res;
}

template <typename Orig, typename Nat>
static Result TestTrig(const char* name, uint64_t n, Orig orig, Nat nat, const Tables& t) {
  Result res; g_cur = name;
  for (uint64_t i = 0; i < n; ++i) {
    PPCContext in = Fresh();
    in.r1.u64 = kStack; in.r3.u64 = 3; in.r4.u64 = 4; in.f1.u64 = std::bit_cast<uint64_t>(RandD(true));
    if (R() % 4 == 0) in.fpscr.csr = uint32_t(R()) & 0x1F;  // vary csr (flush mode bit handling)
    for (uint32_t k = 0; k < kWatchLen; k += 8) Put64(kWatchLo + k, R());
    auto m0 = Snap(kWatchLo, kWatchLen);
    PPCContext a = in, b = in;
    orig(a, memory);
    auto ma = Snap(kWatchLo, kWatchLen);
    std::copy(m0.begin(), m0.end(), memory + kWatchLo);
    nat(b, memory, t);
    auto mb = Snap(kWatchLo, kWatchLen);
    ++res.calls;
    bool ok = ma == mb && SameCtx(a, b, true, false) && UntouchedOk(in, b) && a.r3.u64 == in.r3.u64;
    if (!ok) { ++res.mismatches; Fail("mem/regs", in); }
  }
  return res;
}

static Result TestFloor(uint64_t n, const Tables& t) {
  Result res; g_cur = "floor";
  for (uint64_t i = 0; i < n; ++i) {
    PPCContext in = Fresh();
    in.r1.u64 = kStack; in.r6.u64 = 6; in.f1.u64 = std::bit_cast<uint64_t>(RandD(false));
    if (R() % 4 == 0) in.fpscr.csr = uint32_t(R()) & 0x1F;
    for (uint32_t k = 0; k < kWatchLen; k += 8) Put64(kWatchLo + k, R());
    auto m0 = Snap(kWatchLo, kWatchLen);
    PPCContext a = in, b = in;
    __imp__sub_829160B8(a, memory);
    auto ma = Snap(kWatchLo, kWatchLen);
    std::copy(m0.begin(), m0.end(), memory + kWatchLo);
    Sub829160B8(b, t);
    auto mb = Snap(kWatchLo, kWatchLen);
    ++res.calls;
    bool ok = ma == mb && m0 == ma && SameCtx(a, b, true, true) && UntouchedOk(in, b);
    if (!ok) { ++res.mismatches; Fail("mem/regs", in); }
  }
  return res;
}

// ---------------------------------------------------------------- benchmarks
using Clock = std::chrono::steady_clock;
template <typename F> static double MedianNs(F&& body, uint64_t calls) {
  std::vector<double> runs;
  for (int r = 0; r < 9; ++r) {
    auto t0 = Clock::now();
    body();
    auto t1 = Clock::now();
    runs.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / double(calls));
  }
  std::sort(runs.begin(), runs.end());
  return runs[runs.size() / 2];
}

static void Bench(const Tables& t) {
  constexpr uint64_t N = 1 << 20;
  volatile uint64_t sink = 0;
  std::printf("BENCH (ns/call, median of 9 runs, %llu calls each, inputs pre-generated)\n", (unsigned long long)N);
  // plane test: bounded planes that all lie in front (result 0 after all planes) = worst/typical full loop.
  for (uint32_t planes : {1u, 4u, 6u, 8u}) {
    // 64 point/plane sets, every plane passes (d > 0) so the whole loop executes; and one set exiting at the 1st plane.
    for (int variant = 0; variant < 2; ++variant) {
      constexpr uint32_t kSets = 64;
      for (uint32_t s = 0; s < kSets; ++s) {
        uint32_t base_p = kPlanes + s * 256;
        for (uint32_t p = 0; p < planes; ++p) {
          PutF(base_p + 20 * p + 0, float(U01() - 0.5)); PutF(base_p + 20 * p + 4, float(U01() - 0.5));
          PutF(base_p + 20 * p + 8, float(U01() - 0.5)); PutF(base_p + 20 * p + 12, variant == 0 ? 1000.0f : -1000.0f);
        }
      }
      PPCContext c = Fresh(); c.r3.u64 = kQuatIn; c.r6.s64 = planes; c.r1.u64 = kStack; c.f1.f64 = 0.0;
      for (int k = 0; k < 3; ++k) PutF(kQuatIn + 4 * k, float(U01()));
      auto run = [&](bool native) {
        return MedianNs([&] {
          for (uint64_t i = 0; i < N; ++i) {
            c.r3.u64 = kQuatIn; c.r6.s64 = planes;  // the original consumes r3 and r6
            c.r5.u64 = kPlanes + (i % kSets) * 256;
            if (native) Sub8289B6F0(c, memory); else __imp__sub_8289B6F0(c, memory);
            sink = sink + c.r3.u64;
          }
        }, N);
      };
      double o = run(false), nn = run(true);
      std::printf("plane_test planes=%u %s: orig %.2f native %.2f speedup %.2fx\n", planes,
                  variant == 0 ? "all-pass (full loop)" : "first plane rejects", o, nn, o / nn);
    }
  }
  // quat
  {
    std::vector<std::array<float, 4>> q(1024);
    for (auto& v : q) for (auto& f : v) f = float(U01() - 0.5);
    PPCContext c = Fresh(); c.r1.u64 = kStack; c.r3.u64 = kQuatIn; c.r4.u64 = kQuatOut;
    auto run = [&](bool native) {
      return MedianNs([&] {
        for (uint64_t i = 0; i < N; ++i) {
          const auto& v = q[i & 1023];
          for (int k = 0; k < 4; ++k) PutF(kQuatIn + 4 * k, v[k]);
          if (native) Sub8248B820(c, memory); else __imp__sub_8248B820(c, memory);
        }
        sink = sink + Load<uint32_t>(memory, kQuatOut);
      }, N);
    };
    double o = run(false), nn = run(true);
    // subtract the cost of the 4 input stores, measured alone
    double base = MedianNs([&] {
      for (uint64_t i = 0; i < N; ++i) { const auto& v = q[i & 1023]; for (int k = 0; k < 4; ++k) PutF(kQuatIn + 4 * k, v[k]); }
    }, N);
    std::printf("quat_matrix: orig %.2f native %.2f (input-store overhead %.2f included in both) speedup %.2fx\n", o, nn, base, o / nn);
  }
  std::vector<double> xs(1024);
  auto fill = [&](int mode) { for (auto& x : xs) x = mode == 0 ? (U01() - 0.5) * 12.566 : mode == 1 ? (U01() - 0.5) * 1000 : (U01() - 0.5) * 20; };
  auto trig = [&](const char* name, auto orig, auto nat) {
    for (int mode = 0; mode < 2; ++mode) {
      fill(mode);
      PPCContext c = Fresh(); c.r1.u64 = kStack;
      auto run = [&](bool native) {
        return MedianNs([&] {
          for (uint64_t i = 0; i < N; ++i) {
            c.f1.f64 = xs[i & 1023];
            if (native) nat(c, memory, t); else orig(c, memory);
            sink = sink + c.f1.u64;
          }
        }, N);
      };
      double o = run(false), nn = run(true);
      std::printf("%s |x|<%s: orig %.2f native %.2f speedup %.2fx\n", name, mode == 0 ? "6.3" : "500", o, nn, o / nn);
    }
  };
  trig("sin", [](PPCContext& c, uint8_t* b) { __imp__sub_829170B0(c, b); },
       [](PPCContext& c, uint8_t* b, const Tables& tt) { Sub829170B0(c, b, tt); });
  trig("cos", [](PPCContext& c, uint8_t* b) { __imp__sub_82917190(c, b); },
       [](PPCContext& c, uint8_t* b, const Tables& tt) { Sub82917190(c, b, tt); });
  {
    fill(1);
    for (auto& x : xs) x *= 0.01;
    PPCContext c = Fresh(); c.r1.u64 = kStack;
    auto run = [&](bool native) {
      return MedianNs([&] {
        for (uint64_t i = 0; i < N; ++i) {
          c.f1.f64 = xs[i & 1023];
          if (native) Sub829160B8(c, t); else __imp__sub_829160B8(c, memory);
          sink = sink + c.f1.u64;
        }
      }, N);
    };
    double o = run(false), nn = run(true);
    std::printf("floor: orig %.2f native %.2f speedup %.2fx\n", o, nn, o / nn);
  }
  (void)sink;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const char* image = argc > 1 ? argv[1] : nullptr;
  bool bench = argc > 2 && std::string(argv[2]) == "bench";
  void* m = mmap(nullptr, kMemSize, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) { std::perror("mmap"); return 2; }
  memory = static_cast<uint8_t*>(m);
  auto commit = [&](uint32_t a, size_t n) { if (mprotect(memory + a, n, PROT_READ | PROT_WRITE) != 0) { std::perror("mprotect"); std::exit(2); } };
  commit(0x00100000, 0x200000);
  commit(0x82000000, 0x10000);
  commit(0x82A9C000, 0x2000);        // trig table + NaN constant
  commit(0x8210C000, 0x4000);
  LoadConstants(image);
  Tables t = LoadTables(memory);
  if (bench) { Bench(t); return 0; }
  uint64_t n = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : kCalls;
  Result r[5];
  r[0] = TestPlane(n); r[1] = TestQuat(n);
  r[2] = TestTrig("sin", n, [](PPCContext& c, uint8_t* b) { __imp__sub_829170B0(c, b); },
                  [](PPCContext& c, uint8_t* b, const Tables& tt) { Sub829170B0(c, b, tt); }, t);
  r[3] = TestTrig("cos", n, [](PPCContext& c, uint8_t* b) { __imp__sub_82917190(c, b); },
                  [](PPCContext& c, uint8_t* b, const Tables& tt) { Sub82917190(c, b, tt); }, t);
  r[4] = TestFloor(n, t);
  const char* names[] = {"sub_8289B6F0", "sub_8248B820", "sub_829170B0", "sub_82917190", "sub_829160B8"};
  uint64_t bad = 0;
  for (int i = 0; i < 5; ++i) { Report(names[i], r[i]); bad += r[i].mismatches; }
  std::printf(bad ? "FAIL\n" : "PASS\n");
  return bad ? 1 : 0;
}
