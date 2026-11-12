// Offline harness for Skin4 (app/src/bo2zm_native_skin.cpp) against the generated sub_829143D0.
// tools/tests/test_native_skin4.py splices in the generated body (CONTROL_BODY) and the production skinning file
// (PRODUCTION_SKIN). Synthetic data only; no game files, no game process.
//   native_skin4_test test <calls> <seed>      randomised comparison, prints calls and mismatches
//   native_skin4_test bench <vertices> <runs>  ns per call, original and Skin4, median of <runs>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <sys/mman.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ppc/intrinsics.h>
#include <simde/x86/sse4.1.h>

uint8_t* memory;
template <typename T> T Load(uint8_t* base, uint32_t address) {
  T value;
  std::memcpy(&value, base + address, sizeof(value));
  if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
  else if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
  else return __builtin_bswap64(value);
}
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_GUEST_BARRIER() ((void)0)
#define REX_LOAD_U16(a) Load<uint16_t>(base, uint32_t(a))
#define REX_LOAD_U32(a) Load<uint32_t>(base, uint32_t(a))
#define REX_RAW_ADDR(a) (base + uint32_t(a))
#define DEFINE_REX_FUNC(name) extern "C" REX_FUNC(__imp__##name)

// What the production file takes from the rest of the app.
#define BO2_GAME_ADDR(zombies, campaign) (zombies)
namespace rex::cvar { enum class Lifecycle { kInitOnly }; }
struct CvarStub {
  long v;
  CvarStub& range(int, int) { return *this; }
  CvarStub& lifecycle(rex::cvar::Lifecycle) { return *this; }
};
#define REXCVAR_DEFINE_BOOL(name, def, cat, doc) CvarStub cvar_##name = CvarStub{def}
#define REXCVAR_DEFINE_INT32(name, def, cat, doc) CvarStub cvar_##name = CvarStub{def}
#define REXCVAR_GET(name) (cvar_##name.v)
#define REXLOG_INFO(...) ((void)0)
namespace bo2zm::benchmark { inline int AbArm() { return -1; } }
// The sibling kernels' originals are only referenced by Skin1..3, which this harness never calls.
extern "C" void __imp__sub_82912D78(PPCContext&, uint8_t*) { std::abort(); }
extern "C" void __imp__sub_82913378(PPCContext&, uint8_t*) { std::abort(); }
extern "C" void __imp__sub_82913B50(PPCContext&, uint8_t*) { std::abort(); }

// PRODUCTION_SKIN
// CONTROL_BODY

namespace {
constexpr uint32_t kArena = 0x100000, kArenaSize = 0x80000;  // compared whole after every call
constexpr uint32_t kOutAt = kArena + 0x1000, kVertAt = kArena + 0x10000, kRecAt = kArena + 0x1A000,
                   kBoneAt = kArena + 0x20000, kScaleAt = kArena + 0x7F000;
// The game's constants (the addresses Skin4 reads) in pages of their own, also compared.
constexpr uint32_t kPages[] = {0x82058000, 0x83C28000, 0x84BF0000};  // 16 KB host pages
constexpr uint32_t kPageSize = 0x4000;

std::mt19937_64 rng;
uint32_t R(uint32_t n) { return n ? uint32_t(rng() % n) : 0; }
bool Chance(double p) { return double(rng() >> 11) * 0x1p-53 < p; }
float Uniform(float lo, float hi) { return lo + float(double(rng() >> 11) * 0x1p-53) * (hi - lo); }

void PutWord(uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(memory + address, &v, 4);
}
void PutFloat(uint32_t address, float f) { PutWord(address, std::bit_cast<uint32_t>(f)); }
void PutVec(uint32_t address, float a, float b, float c, float d) {
  PutFloat(address, a); PutFloat(address + 4, b); PutFloat(address + 8, c); PutFloat(address + 12, d);
}

// A float: mostly ordinary, with the density of specials set per call.
float Gen(double special, float range) {
  if (!Chance(special)) return Uniform(-range, range);
  switch (R(9)) {
    case 0: return std::bit_cast<float>(uint32_t(R(1u << 23)) | (R(2) << 31));              // denormal
    case 1: return Uniform(-1, 1) * 1e-30f;                                                  // tiny
    case 2: return Uniform(-1, 1) * 1e19f;                                                   // large
    case 3: return std::bit_cast<float>(0x7F800000u | (R(2) << 31));                         // inf
    case 4: return std::bit_cast<float>(0x7FC00000u);  // the one quiet NaN (see the note on NaN payloads at the top)
    case 5: return R(2) ? 0.0f : -0.0f;
    case 6: return float(int(R(9)) - 4);
    case 7: return std::bit_cast<float>(uint32_t(R(1u << 23)) | 0x00800000u);                // smallest normals
    default: return Uniform(-1, 1) * 3e38f;
  }
}

struct Config {
  bool general_weights = false;   // a weight vperm the fast form cannot take
  bool general_position = false;  // a position vperm the fast form cannot take
  bool fast_cvar = true;
};

void SetupConstants(const Config& c) {
  const float k11 = 4194304.0f / 511.0f;  // T and N: (3.0 + a * 2^-22) * K11 + K10 = a / 511
  PutVec(0x8205A780, 0x1p-22f * 511.0f, 0x1p-22f * 511.0f, 0x1p-22f * 511.0f, 0x1p-22f * 511.0f);  // K8
  PutVec(0x8205A790, 3.0f, 3.0f, 3.0f, 1.0f);                                                          // K9
  PutVec(0x8205A7A0, k11, k11, k11, 1.0f);                                                             // K11
  PutVec(0x8205A7B0, -3.0f * k11, -3.0f * k11, -3.0f * k11, 0.0f);                                     // K10
  PutVec(0x8205A2C0, 1.0f, 1.0f, 1.0f, 1.0f);                                                          // one
  std::memset(memory + 0x8205A2A0, 0, 16);                                                             // weight fill
  uint8_t perm[16] = {0x10, 0x10, 0x00, 0x01, 0x10, 0x10, 0x00, 0x01, 0x10, 0x10, 0x00, 0x01, 0x10, 0x10, 0x10, 0x10};
  if (c.general_weights) {
    // Bytes from the fill (made nonzero) or the weight's own two bytes, in any arrangement.
    for (int i = 0; i < 16; ++i) memory[0x8205A2A0 + i] = uint8_t(R(256));
    for (int i = 0; i < 16; ++i) perm[i] = R(2) ? uint8_t(16 + R(16)) : uint8_t(R(2));
  }
  std::memcpy(memory + 0x84BF3540, perm, 16);
  uint8_t pos[16];
  for (int i = 0; i < 12; ++i) pos[i] = uint8_t(i);
  for (int i = 12; i < 16; ++i) pos[i] = uint8_t(0x1C + i - 12);
  if (c.general_position) {
    for (int i = 0; i < 16; ++i) pos[i] = uint8_t(R(32));
  }
  std::memcpy(memory + 0x83C28900, pos, 16);
}

struct Call {
  PPCContext ctx{};
  uint32_t count = 0;
};

// Fills the arena and ctx for one call.
Call Setup(uint32_t count, bool wide_bones) {
  Call call;
  call.count = count;
  // Junk everywhere: ordinary floats, so that an unaligned lvx that reads a few bytes before a block finds no NaN
  // payload (two NaNs of different payloads in one operation come out as whichever operand the compiler put first,
  // and the original and Skin4 are compiled separately). Both functions are still compared byte for byte.
  for (uint32_t i = 0; i < kArenaSize; i += 4) PutFloat(kArena + i, Uniform(-2, 2));

  const double special = Chance(0.5) ? 0.0 : (Chance(0.5) ? 0.003 : 0.04);
  // r3: vertices, 8-aligned except now and then (the hook then runs the game's own function).
  uint32_t r3 = kVertAt + (Chance(0.93) ? 8 * R(3) : R(8));
  // r7: aligned, or 8 past, or anything.
  uint32_t r7 = kOutAt + 16 * R(4) + (Chance(0.8) ? (Chance(0.5) ? 0 : 8) : R(16));
  uint32_t r4 = kRecAt + 2 * R(8) + (Chance(0.96) ? 0 : 1);
  uint32_t r6 = kBoneAt + (Chance(0.8) ? 16 * R(4) : 4 * R(16));
  uint32_t r8 = kScaleAt + (Chance(0.8) ? 0 : 4 * R(8));
  for (uint32_t i = 0; i < 24 * count; ++i) memory[r3 + i] = uint8_t(rng());  // vertex bytes: positions, unpack fields
  // Positions: s16 x4, sometimes extreme; the bytes 8-15 stay random.
  const float scale = Gen(0, 1) * 0x1p-12f, bias = Gen(0, 1);
  for (int i = 0; i < 4; ++i) {
    PutFloat(r8 + 4 * i, Chance(special) ? Gen(1, 1) : scale);
    PutFloat(r8 + 16 + 4 * i, Chance(special) ? Gen(1, 1) : bias);
  }
  // Bone matrices: 64 bytes each, offsets are u16 from r6.
  const uint32_t bone_bytes = wide_bones ? 0xFFF0 + 64 : 4096;
  for (uint32_t i = 0; i < bone_bytes; i += 4) PutFloat(r6 + i, Gen(special, 2.0f));
  // Records: 14 bytes, big-endian halfwords: b0, b1, w1, b2, w2, b3, w3.
  const uint32_t modes = R(4);  // 0 distinct, 1 runs of equal records, 2 all equal, 3 distinct with small bone set
  uint8_t record[14] = {};
  uint32_t run = 0;
  auto bone = [&]() -> uint32_t {
    uint32_t off = wide_bones ? (R(0xFFF0 / 64) * 64) : (R(60) * 64);
    if (modes == 3) off = R(4) * 64;
    if (Chance(0.03)) off += 4 * R(16);  // an offset that is not a multiple of 64: lvx rounds down
    return std::min<uint32_t>(off, wide_bones ? 0xFFF0 : 4096 - 64);
  };
  auto weight = [&](int kind) -> uint32_t {
    switch (kind) {
      case 0: return R(65536);
      case 1: return R(2) ? 0 : 0xFFFF;
      default: return R(21846);  // three of them sum to at most ~1
    }
  };
  for (uint32_t i = 0; i < count; ++i) {
    if (run == 0 || modes == 0 || modes == 3) {
      const int kind = int(R(3));
      uint32_t h[7] = {bone(), bone(), weight(kind), bone(), weight(kind), bone(), weight(kind)};
      for (int j = 0; j < 7; ++j) {
        record[2 * j] = uint8_t(h[j] >> 8);
        record[2 * j + 1] = uint8_t(h[j]);
      }
      run = modes == 1 ? 1 + R(6) : (modes == 2 ? ~0u : 1);
    }
    --run;
    std::memcpy(memory + r4 + 14 * i, record, 14);
  }
  call.ctx.r3.u64 = r3;
  call.ctx.r4.u64 = r4;
  call.ctx.r5.u64 = count;
  call.ctx.r6.u64 = r6;
  call.ctx.r7.u64 = r7;
  call.ctx.r8.u64 = r8;
  call.ctx.lr = 0x82915000;
  return call;
}

int RunTest(long calls, uint64_t seed) {
  static const uint32_t kCounts[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100,
                                     127, 128, 129, 255, 256, 257, 500, 999, 1000};
  rng.seed(seed);
  std::vector<uint8_t> saved(kArenaSize), expected(kArenaSize);
  std::vector<uint8_t> pages_before(sizeof(kPages) / sizeof(kPages[0]) * kPageSize);
  long mismatches = 0, fallbacks = 0, empty = 0, odd_records = 0;
  long variants[4] = {};
  for (long n = 0; n < calls; ++n) {
    Config cfg;
    const uint32_t variant = n % 8 < 4 ? 0 : (n % 8 < 6 ? 1 : (n % 8 == 6 ? 2 : 3));
    cfg.general_weights = variant == 2 || variant == 3;
    cfg.general_position = variant == 3;
    cfg.fast_cvar = variant != 1;
    ++variants[variant];
    cvar_bo2zm_native_skin_fast.v = cfg.fast_cvar;
    SetupConstants(cfg);
    const uint32_t count = Chance(0.5) ? kCounts[R(sizeof(kCounts) / sizeof(kCounts[0]))] : R(1001);
    Call call = Setup(count, Chance(0.02));
    fallbacks += (call.ctx.r3.u32 & 7) != 0;
    empty += count == 0;
    odd_records += (call.ctx.r4.u32 & 1) != 0;
    for (size_t p = 0; p < sizeof(kPages) / sizeof(kPages[0]); ++p)
      std::memcpy(&pages_before[p * kPageSize], memory + kPages[p], kPageSize);
    std::memcpy(saved.data(), memory + kArena, kArenaSize);
    Call orig = call;
    __imp__sub_829143D0(orig.ctx, memory);
    std::memcpy(expected.data(), memory + kArena, kArenaSize);
    std::memcpy(memory + kArena, saved.data(), kArenaSize);
    Call mine = call;
    bo2zm::native_skin::Skin4(mine.ctx, memory);
    bool bad = std::memcmp(expected.data(), memory + kArena, kArenaSize) != 0 || orig.ctx.r3.u64 != mine.ctx.r3.u64;
    for (size_t p = 0; p < sizeof(kPages) / sizeof(kPages[0]); ++p)
      bad |= std::memcmp(&pages_before[p * kPageSize], memory + kPages[p], kPageSize) != 0;
    if (bad) {
      ++mismatches;
      if (mismatches <= 5) {
        size_t first = 0;
        while (first < kArenaSize && expected[first] == memory[kArena + first]) ++first;
        std::printf("MISMATCH call %ld variant %u count %u r3 %08X r4 %08X r6 %08X r7 %08X: first byte %zx (arena+) "
                    "orig %02X mine %02X, r3 orig %llX mine %llX\n",
                    n, variant, count, call.ctx.r3.u32, call.ctx.r4.u32, call.ctx.r6.u32, call.ctx.r7.u32, first,
                    first < kArenaSize ? expected[first] : 0, first < kArenaSize ? memory[kArena + first] : 0,
                    (unsigned long long)orig.ctx.r3.u64, (unsigned long long)mine.ctx.r3.u64);

        std::printf("  first diff: vertex %zu byte %zu of 32 (out base %X)\n", (kArena + first - (call.ctx.r7.u32 & ~15u)) / 32,
                    (kArena + first - (call.ctx.r7.u32 & ~15u)) % 32, call.ctx.r7.u32 & ~15u);
      }
    }
  }
  std::printf("calls %ld mismatches %ld (variants fast %ld, general-by-cvar %ld, general weights %ld, general weights+position "
              "%ld; unaligned-vertex fallbacks %ld, empty %ld, odd record arrays %ld)\n",
              calls, mismatches, variants[0], variants[1], variants[2], variants[3], fallbacks, empty, odd_records);
  return mismatches ? 1 : 0;
}

double NsPerCall(void (*fn)(PPCContext&, uint8_t*), const Call& call, int iterations, std::vector<uint8_t>& saved) {
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    Call c = call;
    fn(c.ctx, memory);
    asm volatile("" ::: "memory");
  }
  const auto t1 = std::chrono::steady_clock::now();
  (void)saved;
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / iterations;
}

int RunBench(uint32_t count, int runs) {
  rng.seed(12345);
  std::vector<uint8_t> unused;
  for (int clustered = 0; clustered < 2; ++clustered) {
    Config cfg;
    cvar_bo2zm_native_skin_fast.v = 1;
    SetupConstants(cfg);
    Call call = Setup(count, false);
    // Regular data: 8-aligned vertices, even records, aligned output; the records are runs of 4 equal ones
    // (clustered) or all different.
    call.ctx.r3.u64 = kVertAt;
    call.ctx.r4.u64 = kRecAt;
    call.ctx.r7.u64 = kOutAt;
    call.ctx.r6.u64 = kBoneAt;
    call.ctx.r8.u64 = kScaleAt;
    if (clustered) {
      for (uint32_t i = 0; i < count; ++i) std::memcpy(memory + kRecAt + 14 * i, memory + kRecAt + 14 * (i & ~3u), 14);
    }
    // Plain numbers: no specials in matrices (keep denormal-free arithmetic, as real data is).
    const int iterations = std::max<int>(200, 400000 / int(count + 8));
    std::vector<double> o, m;
    for (int r = 0; r < runs; ++r) {
      o.push_back(NsPerCall(__imp__sub_829143D0, call, iterations, unused));
      m.push_back(NsPerCall(bo2zm::native_skin::Skin4, call, iterations, unused));
    }
    std::sort(o.begin(), o.end());
    std::sort(m.begin(), m.end());
    std::printf("BENCH vertices %u records %s: original %.1f ns/call (%.2f ns/vertex), Skin4 %.1f ns/call (%.2f ns/vertex), "
                "x%.2f (median of %d runs x %d calls)\n",
                count, clustered ? "runs of 4 equal" : "all different", o[runs / 2], o[runs / 2] / count, m[runs / 2],
                m[runs / 2] / count, o[runs / 2] / m[runs / 2], runs, iterations);
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  const size_t reservation = 0x85000000ull;
  memory = static_cast<uint8_t*>(mmap(nullptr, reservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (memory == MAP_FAILED) return 2;
  if (mprotect(memory + kArena, kArenaSize, PROT_READ | PROT_WRITE)) return 2;
  for (uint32_t p : kPages) {
    if (mprotect(memory + p, kPageSize, PROT_READ | PROT_WRITE)) return 2;
  }
  // The window around the arena: the original's two-wide loads may touch the 16 bytes after a region; leave
  // neighbours mapped.
  mprotect(memory + kArena - 0x4000, 0x4000, PROT_READ | PROT_WRITE);
  mprotect(memory + kArena + kArenaSize, 0x4000, PROT_READ | PROT_WRITE);
  if (argc >= 4 && !std::strcmp(argv[1], "test")) return RunTest(std::atol(argv[2]), std::strtoull(argv[3], nullptr, 0));
  if (argc >= 4 && !std::strcmp(argv[1], "bench")) return RunBench(uint32_t(std::atoi(argv[2])), std::atoi(argv[3]));
  std::fprintf(stderr, "usage: native_skin4_test test <calls> <seed> | bench <vertices> <runs>\n");
  return 2;
}
