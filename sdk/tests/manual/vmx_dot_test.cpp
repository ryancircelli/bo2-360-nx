// Standalone regression: compile with the SDK and simde include directories.
#include <rex/ppc/intrinsics.h>
#include <cassert>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>

// The portable scalar VmxDot (79d682ed), kept here as the reference for the NEON path.
template <int Elements>
static uint32_t reference_dot(const float av[4], const float bv[4]) {
  double products[4]{};
  for (int i = 0; i < Elements; ++i) {
    uint32_t ab, bb;
    std::memcpy(&ab, &av[3 - i], sizeof(ab));
    std::memcpy(&bb, &bv[3 - i], sizeof(bb));
    if ((ab & 0x7F800000u) == 0) ab &= 0x80000000u;
    if ((bb & 0x7F800000u) == 0) bb &= 0x80000000u;
    float af, bf;
    std::memcpy(&af, &ab, sizeof(af));
    std::memcpy(&bf, &bb, sizeof(bf));
    products[i] = double(af) * double(bf);
  }
  const double sum = Elements == 3 ? (products[0] + products[2]) + products[1]
                                   : (products[0] + products[2]) + (products[1] + products[3]);
  const float result = float(sum);
  uint32_t bits;
  std::memcpy(&bits, &result, sizeof(bits));
  if (std::isnan(result) || (std::isinf(result) && !std::isinf(sum)))
    bits = 0x7FC00000u;
  else if ((bits & 0x7F800000u) == 0)
    bits &= 0x80000000u;
  return bits;
}

// Random words biased to the edges: zeros, denormals, FLT_MAX region, inf, NaN, near-cancelling values.
static uint32_t edge_word(std::mt19937_64& rng) {
  static const uint32_t specials[] = {0, 0x80000000u, 1, 0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
                                      0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u, 0x7F800001u,
                                      0x3F800000u, 0xBF800000u, 0x5F800000u, 0x1F800000u};
  const uint64_t r = rng();
  switch (r % 4) {
    case 0: return specials[(r >> 8) % (sizeof(specials) / sizeof(specials[0]))];
    case 1: return uint32_t(r >> 32);  // any bit pattern
    case 2: return (uint32_t(r >> 32) & 0x807FFFFFu) | (uint32_t((r >> 16) % 8 + 0xF8) << 23);  // huge
    default: return (uint32_t(r >> 32) & 0x807FFFFFu) | (uint32_t((r >> 16) % 16) << 23);  // tiny/denormal
  }
}

template <int Elements>
static uint64_t compare_random(uint64_t cases, uint64_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t bad = 0;
  for (uint64_t n = 0; n < cases; ++n) {
    alignas(16) uint32_t a[4], b[4];
    const bool edges = n & 1;
    for (int i = 0; i < 4; ++i) {
      a[i] = edges ? edge_word(rng) : uint32_t(rng());
      b[i] = edges ? edge_word(rng) : uint32_t(rng());
    }
    if (n % 3 == 0) {  // b = a with flipped signs in some lanes: cancellation
      for (int i = 0; i < 4; ++i) b[i] = a[i] ^ (uint32_t(rng() & 1) << 31);
    }
    alignas(16) float af[4], bf[4], out[4];
    std::memcpy(af, a, 16);
    std::memcpy(bf, b, 16);
    simde_mm_store_ps(out, rex::ppc::VmxDot<Elements>(simde_mm_load_ps(af), simde_mm_load_ps(bf)));
    const uint32_t want = reference_dot<Elements>(af, bf);
    for (float lane : out) {
      uint32_t got;
      std::memcpy(&got, &lane, 4);
      if (got != want && bad++ < 10)
        printf("MISMATCH dot%d a=%08x %08x %08x %08x b=%08x %08x %08x %08x got %08x want %08x\n", Elements,
               a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], got, want);
    }
  }
  return bad;
}

static simde__m128 guest(float x, float y, float z, float w) {
  return simde_mm_set_ps(x, y, z, w);
}
static void check(simde__m128 v, float expected) {
  alignas(16) float lanes[4];
  simde_mm_store_ps(lanes, v);
  for (float lane : lanes) assert(std::isnan(expected) ? std::isnan(lane) : lane == expected);
}
int main() {
  const auto sentinel = guest(-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX);
  const auto old_dot = simde_mm_dp_ps(sentinel, sentinel, 0xEF);
  check(old_dot, std::numeric_limits<float>::infinity());
  check(rex::ppc::VmxDot<3>(sentinel, sentinel), std::numeric_limits<float>::quiet_NaN());
  // Infinity makes the first GJK improvement test true; Xenon's NaN keeps it false.
  const auto previous = simde_mm_set1_ps(FLT_MAX);
  const auto tolerance = simde_mm_set1_ps(0.01f);
  auto stop = [&](simde__m128 dot) {
    return simde_mm_movemask_ps(simde_mm_cmpge_ps(simde_mm_mul_ps(previous, tolerance),
                                               simde_mm_sub_ps(previous, dot)));
  };
  assert(stop(old_dot) == 15);
  assert(stop(rex::ppc::VmxDot<3>(sentinel, sentinel)) == 0);
  check(rex::ppc::VmxDot<3>(guest(1,2,3,100), guest(4,5,6,100)), 32);
  check(rex::ppc::VmxDot<4>(guest(1,2,3,100), guest(4,5,6,100)), 10032);
  // Products may exceed float range and cancel before the one final narrowing.
  check(rex::ppc::VmxDot<3>(guest(FLT_MAX,2,FLT_MAX,0), guest(2,3,-2,0)), 6);
  const float inf = std::numeric_limits<float>::infinity();
  check(rex::ppc::VmxDot<3>(guest(inf,1,1,0), guest(1,1,1,0)), inf);
  check(rex::ppc::VmxDot<3>(guest(inf,1,-inf,0), guest(1,1,1,0)),
        std::numeric_limits<float>::quiet_NaN());
  check(rex::ppc::VmxDot<3>(guest(1e20f,3,-1e20f,0), guest(1,1,1,0)), 3);
  check(rex::ppc::VmxDot<3>(guest(FLT_MIN,0,0,0), guest(.5f,0,0,0)), 0);
  const uint64_t cases = getenv("VMX_DOT_CASES") ? strtoull(getenv("VMX_DOT_CASES"), nullptr, 10) : 4000000;
  uint64_t bad = 0;
#if defined(__aarch64__)
  // Every FPCR rounding mode, with and without flush-to-zero (the generated code sets FZ around vector math).
  uint64_t fpcr0;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr0));
  for (uint64_t mode = 0; mode < 8; ++mode) {
    const uint64_t fpcr = (fpcr0 & ~((3ull << 22) | (1ull << 24))) | ((mode & 3) << 22) | ((mode >> 2) << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
    bad += compare_random<3>(cases / 8, 1 + 2 * mode) + compare_random<4>(cases / 8, 2 + 2 * mode);
  }
  __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr0));
#else
  bad = compare_random<3>(cases, 1) + compare_random<4>(cases, 2);
#endif
  printf("random: %llu cases per width, %llu mismatches against the scalar reference\n",
         (unsigned long long)cases, (unsigned long long)bad);
  assert(bad == 0);
  puts("PASS: dot overflow, collision stop condition, lane order, cancellation, infinities, denormals");
}
