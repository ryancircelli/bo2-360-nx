// See bo2zm_gpu_skin.h.

#include "bo2zm_gpu_skin.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "bo2zm_benchmark.h"

// On by default on the Switch: together with plain guest memory, the native sound limiter and the entity loops it
// raised the frame rate at round 15 from 40.9 to 43.3 FPS (+5.8 %) on an OLED. Not tried on the PC yet, so off there.
#if defined(__SWITCH__)
constexpr bool kGpuSkinDefault = true;
#else
constexpr bool kGpuSkinDefault = false;
#endif
REXCVAR_DEFINE_BOOL(bo2zm_gpu_skin, kGpuSkinDefault, "BO2",
                    "Native renderer: the three CPU skinning kernels become GPU work. Their hooks record "
                    "each call and the renderer skins the vertices with a compute pass where it would copy them "
                    "(bo2zm_gpu_skin.h). false = skinned on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_gpu_skin_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_gpu_skin (on = the GPU, "
                     "off = the CPU). 0 = not part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_gpu_skin_check, 0, "BO2",
                     "Test, with bo2zm_gpu_skin: this many jobs also run the CPU kernel, and the renderer compares its "
                     "output with the GPU's once the GPU is done ([gpu_skin] lines). 0 = none")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::gpu_skin {
namespace {

// The jobs, by 16 KB region of their output. A job has an entry in the shard of each region it reaches
// (region % kShards); each shard has its own lock and a short list, and the jobs of different surfaces mostly fall
// in different shards. Nothing is allocated or freed under a lock: the jobs a call drops are released after it
// unlocks, and a shard's list allocates only when it outgrows its largest size so far. The single map under a single
// mutex this replaces had the job workers blocked on it for 15-16 % of their samples and the Backend for 12 %: its
// holders freed jobs and allocated map nodes inside it, so each could wait on the C heap's lock while holding it. With
// 64 KB regions the workers still waited 1.6-1.9 % of their samples in Add, because the surfaces two workers skin at
// the same time are often neighbors in the output; 16 KB regions brought that under 1 %.
constexpr uint32_t kRegionShift = 14;
constexpr uint32_t kShards = 64;
struct Entry {
  uint32_t lo = 0, hi = 0;  // the job's output, [lo, hi)
  JobRef job;
};
struct alignas(64) Shard {
  std::mutex mutex;
  std::vector<Entry> entries;  // live jobs never overlap; forgotten ones' entries may still be here
};
Shard g_shards[kShards];
// One bit per 64 KB region of the 512 MB of physical memory: a job was recorded there once (never cleared).
std::atomic<uint64_t> g_regions[(1u << 13) / 64];
std::atomic<uint64_t> g_serial{0};
std::atomic<int64_t> g_checks_left{-1};
std::atomic<bool> g_ready{false};

// Hook side, for the report.
std::atomic<uint64_t> g_recorded{0}, g_recorded_vertices{0}, g_forgotten{0}, g_not_recorded{0}, g_bone_bytes{0};
std::atomic<int64_t> g_live{0};

RendererCounts g_counts;

// The shards [lo, hi) reaches, one bit each.
uint64_t ShardsOf(uint32_t lo, uint64_t hi) {
  const uint64_t first = lo >> kRegionShift, last = (hi - 1) >> kRegionShift;
  if (last - first + 1 >= kShards) {
    return ~uint64_t(0);
  }
  uint64_t mask = 0;
  for (uint64_t r = first; r <= last; ++r) {
    mask |= uint64_t(1) << (r % kShards);
  }
  return mask;
}

// The shards' locks, taken in index order (so two calls never wait on each other) and given back at the end of the
// scope. Calls that share a shard run one after the other, as with one lock.
class Locked {
 public:
  explicit Locked(uint64_t mask) : mask_(mask) {
    for (uint64_t m = mask_; m; m &= m - 1) {
      g_shards[std::countr_zero(m)].mutex.lock();
    }
  }
  ~Locked() {
    for (uint64_t m = mask_; m; m &= m - 1) {
      g_shards[std::countr_zero(m)].mutex.unlock();
    }
  }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;

 private:
  uint64_t mask_;
};

// The references a call drops, released when it ends: declared before its Locked, so after the locks are given back.
class Dropped {
 public:
  void Add(JobRef&& job) {
    if (n_ < kInline) {
      inline_[n_++] = std::move(job);
    } else {
      more_.push_back(std::move(job));
    }
  }

 private:
  static constexpr size_t kInline = 16;
  JobRef inline_[kInline];
  size_t n_ = 0;
  std::vector<JobRef> more_;
};

void Kill(const Job& job) {
  if (!job.forgotten.exchange(true, std::memory_order_acq_rel)) {
    g_live.fetch_sub(1, std::memory_order_relaxed);
    g_forgotten.fetch_add(1, std::memory_order_relaxed);
  }
}

// Under the shards' locks: the jobs whose output overlaps [lo, hi) are forgotten, and their entries in these shards
// are dropped with any forgotten job's. A forgotten job's entries in other shards wait for those shards' next call.
void KillOverlapping(uint64_t mask, uint32_t lo, uint64_t hi, Dropped& dropped) {
  for (uint64_t m = mask; m; m &= m - 1) {
    std::vector<Entry>& entries = g_shards[std::countr_zero(m)].entries;
    for (size_t i = 0; i < entries.size();) {
      Entry& e = entries[i];
      const bool overlaps = e.lo < hi && e.hi > lo;
      if (overlaps) {
        Kill(*e.job);
      }
      if (overlaps || e.job->forgotten.load(std::memory_order_relaxed)) {
        dropped.Add(std::move(e.job));
        if (i + 1 != entries.size()) {
          e = std::move(entries.back());
        }
        entries.pop_back();
        continue;
      }
      ++i;
    }
  }
}

// The jobs no reference holds, ready for JobRef::New. A short spin lock: a push or a pop is a few instructions.
class Pool {
 public:
  Job* Take() {
    Hold hold(lock_);
    if (free_.empty()) {
      return nullptr;
    }
    Job* job = free_.back();
    free_.pop_back();
    return job;
  }
  void Give(Job* job) {
    Hold hold(lock_);
    if (free_.capacity() == 0) {
      free_.reserve(4096);
    }
    free_.push_back(job);
  }

 private:
  class Hold {
   public:
    explicit Hold(std::atomic<bool>& lock) : lock_(lock) {
      while (lock_.exchange(true, std::memory_order_acquire)) {
        while (lock_.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
      }
    }
    ~Hold() { lock_.store(false, std::memory_order_release); }

   private:
    std::atomic<bool>& lock_;
  };
  std::atomic<bool> lock_{false};
  std::vector<Job*> free_;
};
Pool g_pool;
std::atomic<uint64_t> g_jobs_made{0};

}  // namespace

JobRef JobRef::New() {
  Job* job = g_pool.Take();
  if (!job) {
    job = new Job;
    g_jobs_made.fetch_add(1, std::memory_order_relaxed);
  }
  job->forgotten.store(false, std::memory_order_relaxed);
  job->cpu_written = false;
  job->refs.store(1, std::memory_order_relaxed);
  JobRef ref;
  ref.job_ = job;
  return ref;
}

void JobRef::Release() {
  if (job_ && job_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    g_pool.Give(job_);
  }
  job_ = nullptr;
}

bool Enabled() {
  static const bool on = REXCVAR_GET(bo2zm_gpu_skin) || REXCVAR_GET(bo2zm_gpu_skin_ab) != 0;
  return on;
}

bool RendererOn() {
  return Enabled();
}

void SetRendererReady(bool ready) {
  g_ready.store(ready, std::memory_order_release);
}

bool WantedNow() {
  if (!g_ready.load(std::memory_order_acquire)) {
    return false;
  }
  static const int factor = REXCVAR_GET(bo2zm_gpu_skin_ab);
  if (factor != 0) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  static const bool on = REXCVAR_GET(bo2zm_gpu_skin);
  return on;
}

bool CheckNext() {
  int64_t left = g_checks_left.load(std::memory_order_relaxed);
  if (left < 0) {
    left = REXCVAR_GET(bo2zm_gpu_skin_check);
    int64_t unset = -1;
    g_checks_left.compare_exchange_strong(unset, left);
  }
  if (left == 0) {
    return false;
  }
  return g_checks_left.fetch_sub(1, std::memory_order_relaxed) > 0;
}

bool Physical(uint32_t guest, uint32_t& physical) {
  if (guest < 0xA0000000u) {
    return false;
  }
  // As bo2zm_page_writes.h's MarkGuest: the 0xE0000000 view starts at physical 0x1000.
  physical = (guest & 0x1FFFFFFFu) + (guest >= 0xE0000000u ? 0x1000u : 0u);
  return true;
}

void Forget(uint32_t guest, uint32_t bytes) {
  uint32_t lo = 0;
  if (bytes == 0 || !Physical(guest, lo)) {
    return;
  }
  if (!MayOverlap(lo, bytes)) {
    return;
  }
  const uint64_t hi = uint64_t(lo) + bytes;
  const uint64_t mask = ShardsOf(lo, hi);
  Dropped dropped;
  Locked locked(mask);
  KillOverlapping(mask, lo, hi, dropped);
}

void Add(JobRef job) {
  job->serial = g_serial.fetch_add(1, std::memory_order_relaxed);
  const uint32_t lo = job->out;
  const uint64_t hi = uint64_t(lo) + uint64_t(job->count) * 32;
  for (uint32_t r = lo >> 16; r <= uint32_t((hi - 1) >> 16) && r < (1u << 13); ++r) {
    const uint64_t bit = uint64_t(1) << (r & 63);
    if (!(g_regions[r >> 6].load(std::memory_order_relaxed) & bit)) {
      g_regions[r >> 6].fetch_or(bit, std::memory_order_release);
    }
  }
  g_recorded.fetch_add(1, std::memory_order_relaxed);
  g_recorded_vertices.fetch_add(job->count, std::memory_order_relaxed);
  g_bone_bytes.fetch_add(job->bones.size(), std::memory_order_relaxed);
  g_live.fetch_add(1, std::memory_order_relaxed);
  const uint64_t mask = ShardsOf(lo, hi);
  JobRef held = std::move(job);
  Dropped dropped;
  Locked locked(mask);
  KillOverlapping(mask, lo, hi, dropped);
  for (uint64_t m = mask; m; m &= m - 1) {
    std::vector<Entry>& entries = g_shards[std::countr_zero(m)].entries;
    if (entries.capacity() == 0) {
      entries.reserve(64);
    }
    entries.push_back(Entry{lo, uint32_t(hi), held});
  }
}

void CountNotRecorded() {
  g_not_recorded.fetch_add(1, std::memory_order_relaxed);
}

bool MayOverlap(uint32_t physical, uint32_t bytes) {
  const uint32_t first = (physical & 0x1FFFFFFFu) >> 16;
  const uint32_t last = std::min<uint32_t>(((physical & 0x1FFFFFFFu) + bytes - 1) >> 16, (1u << 13) - 1);
  for (uint32_t r = first; r <= last; ++r) {
    if (g_regions[r >> 6].load(std::memory_order_acquire) & (uint64_t(1) << (r & 63))) {
      return true;
    }
  }
  return false;
}

void Overlapping(uint32_t physical, uint32_t bytes, std::vector<JobRef>& jobs) {
  jobs.clear();
  if (bytes == 0) {
    return;
  }
  const uint64_t hi = uint64_t(physical) + bytes;
  const uint64_t mask = ShardsOf(physical, hi);
  {
    Locked locked(mask);
    for (uint64_t m = mask; m; m &= m - 1) {
      for (const Entry& e : g_shards[std::countr_zero(m)].entries) {
        if (e.lo < hi && e.hi > physical && !e.job->forgotten.load(std::memory_order_relaxed)) {
          jobs.push_back(e.job);
        }
      }
    }
  }
  if (jobs.size() > 1) {
    // By address (live jobs never overlap); a job that reaches two of the shards was found twice.
    std::sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) { return a->out < b->out; });
    jobs.erase(std::unique(jobs.begin(), jobs.end()), jobs.end());
  }
}

namespace {
using V4 = std::array<float, 4>;
inline uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint32_t Be16(const uint8_t* p) {
  return uint32_t(p[0]) << 8 | p[1];
}
inline V4 Fma(const V4& a, const V4& b, const V4& c) {
  return {std::fma(a[0], b[0], c[0]), std::fma(a[1], b[1], c[1]), std::fma(a[2], b[2], c[2]),
          std::fma(a[3], b[3], c[3])};
}
inline V4 Mul(const V4& a, const V4& b) {
  return {a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]};
}
inline V4 Sub(const V4& a, const V4& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]};
}
inline V4 Splat(float f) {
  return {f, f, f, f};
}
inline V4 Vec(const uint32_t* words) {
  return {std::bit_cast<float>(words[0]), std::bit_cast<float>(words[1]), std::bit_cast<float>(words[2]),
          std::bit_cast<float>(words[3])};
}
struct Matrix {
  V4 r0, r1, r2, r3;
};
// The shader's operations (shaders/bo2zm_gpu_skin.comp), on the CPU.
Matrix Bone(const Job& job, uint32_t offset) {
  const uint8_t* at = job.bones.data() + (((job.bones_guest + offset) & ~15u) - job.bones_lo);
  Matrix m;
  V4* rows[4] = {&m.r0, &m.r1, &m.r2, &m.r3};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      (*rows[r])[c] = std::bit_cast<float>(Be32(at + 16 * r + 4 * c));
    }
  }
  return m;
}
V4 Unpack(uint32_t x) {
  const int32_t a = int32_t(x << 22) >> 22, b = int32_t(x << 12) >> 22, c = int32_t(x << 2) >> 22;
  return {std::bit_cast<float>(a == -512 ? 0x7FC00000u : uint32_t(a + 0x40400000)),
          std::bit_cast<float>(uint32_t(b + 0x40400000)), std::bit_cast<float>(uint32_t(c + 0x40400000)),
          std::bit_cast<float>((x >> 30) | 0x3F800000u)};
}
uint32_t Pack(const V4& v) {
  static const uint32_t lo[4] = {0x403FFE01u, 0x403FFE01u, 0x403FFE01u, 0x40400000u};
  static const uint32_t hi[4] = {0x404001FFu, 0x404001FFu, 0x404001FFu, 0x40400003u};
  static const uint32_t mask[4] = {0x3FF, 0x3FF, 0x3FF, 0x3};
  uint32_t word = 0;
  for (int i = 0; i < 4; ++i) {
    const float l = std::bit_cast<float>(lo[i]), h = std::bit_cast<float>(hi[i]);
    float t = v[i] > h ? h : v[i];
    t = v[i] < l ? l : t;
    word |= (std::bit_cast<uint32_t>(t) & mask[i]) << (10 * i);
  }
  return word;
}
V4 Chain(const V4& v, const Matrix& m, bool round_y) {
  const V4 a = round_y ? Fma(Splat(v[2]), m.r2, Mul(Splat(v[1]), m.r1))
                       : Fma(Splat(v[1]), m.r1, Mul(Splat(v[2]), m.r2));
  return Fma(Splat(v[0]), m.r0, a);
}
V4 Weight(uint32_t half, const uint32_t* lanes) {
  const uint32_t w = std::bit_cast<uint32_t>(float(half) * 0x1p-16f);
  return {std::bit_cast<float>(w & lanes[0]), std::bit_cast<float>(w & lanes[1]), std::bit_cast<float>(w & lanes[2]),
          std::bit_cast<float>(w & lanes[3])};
}
}  // namespace

void Materialize(const Job& job) {
  static constexpr uint32_t kHalves[3] = {1, 3, 5};
  const uint32_t* k = job.constants;
  const V4 scale = Vec(k), bias = Vec(k + 4), k8 = Vec(k + 8), k9 = Vec(k + 12), k10 = Vec(k + 16), k11 = Vec(k + 20),
           one = Vec(k + 24);
  const uint32_t* lanes = k + 28;
  for (uint32_t i = 0; i < job.count; ++i) {
    const uint8_t* r = job.records + size_t(i) * kHalves[job.kernel] * 2;
    Matrix m;
    if (job.kernel == 0) {
      m = Bone(job, Be16(r));
    } else if (job.kernel == 1) {
      const Matrix a = Bone(job, Be16(r)), b = Bone(job, Be16(r + 2));
      const V4 wb = Weight(Be16(r + 4), lanes);
      const V4 wa = Sub(one, wb);
      m = {Fma(b.r0, wb, Mul(wa, a.r0)), Fma(b.r1, wb, Mul(wa, a.r1)), Fma(b.r2, wb, Mul(wa, a.r2)),
           Fma(b.r3, wb, Mul(wa, a.r3))};
    } else {
      const Matrix c = Bone(job, Be16(r)), a = Bone(job, Be16(r + 2)), b = Bone(job, Be16(r + 6));
      const V4 wa = Weight(Be16(r + 4), lanes), wb = Weight(Be16(r + 8), lanes);
      const V4 wc = Sub(Sub(one, wb), wa);
      m = {Fma(wc, c.r0, Fma(wa, a.r0, Mul(b.r0, wb))), Fma(wc, c.r1, Fma(wa, a.r1, Mul(b.r1, wb))),
           Fma(wc, c.r2, Fma(wa, a.r2, Mul(b.r2, wb))), Fma(wc, c.r3, Fma(wa, a.r3, Mul(b.r3, wb)))};
    }
    const uint8_t* in = job.vertices + size_t(i) * 24;
    const V4 raw = {float(int16_t(Be16(in))), float(int16_t(Be16(in + 2))), float(int16_t(Be16(in + 4))),
                    float(int16_t(Be16(in + 6)))};
    const V4 p = Fma(raw, scale, bias);
    const V4 t = Fma(Unpack(Be32(in + 16)), k11, k10), n = Fma(Unpack(Be32(in + 20)), k11, k10);
    V4 pa = Fma(m.r2, Splat(p[2]), m.r3);
    pa = Fma(Splat(p[1]), m.r1, pa);
    const V4 pt = Fma(Splat(p[0]), m.r0, pa);
    const uint32_t words[8] = {std::bit_cast<uint32_t>(pt[0]),
                               std::bit_cast<uint32_t>(pt[1]),
                               std::bit_cast<uint32_t>(pt[2]),
                               std::bit_cast<uint32_t>(p[3]),
                               Be32(in + 8),
                               Be32(in + 12),
                               Pack(Fma(Chain(t, m, job.kernel != 1), k8, k9)),
                               Pack(Fma(Chain(n, m, true), k8, k9))};
    uint8_t* o = job.target + size_t(i) * 32;
    for (int w = 0; w < 8; ++w) {
      o[4 * w] = uint8_t(words[w] >> 24);
      o[4 * w + 1] = uint8_t(words[w] >> 16);
      o[4 * w + 2] = uint8_t(words[w] >> 8);
      o[4 * w + 3] = uint8_t(words[w]);
    }
  }
  Kill(job);
}

RendererCounts& Counts() {
  return g_counts;
}

void Refused(const char* reason, uint32_t physical, uint32_t bytes, uint32_t stride, uint32_t order) {
  static std::atomic<int> said{0};
  if (said.fetch_add(1, std::memory_order_relaxed) < 20) {
    REXLOG_WARN("[gpu_skin] a range with jobs copied from guest memory instead ({}): {:08X} +{} bytes, stride {}, "
                "order {}",
                reason, physical, bytes, stride, order);
  }
}

namespace {
// A packed 2:10:10:10 word: each field within one of the other's (10-bit fields signed, the 2-bit w unsigned).
bool PackedNear(uint32_t a, uint32_t b) {
  for (int shift : {0, 10, 20}) {
    const int fa = int32_t(a << (22 - shift)) >> 22;
    const int fb = int32_t(b << (22 - shift)) >> 22;
    if (std::abs(fa - fb) > 1) {
      return false;
    }
  }
  return std::abs(int(a >> 30) - int(b >> 30)) <= 1;
}
bool FloatNear(uint32_t a, uint32_t b) {
  const float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
  if (std::isnan(fa) || std::isnan(fb)) {
    return std::isnan(fa) && std::isnan(fb);
  }
  return std::fabs(fa - fb) <= 1e-5f * std::max(1.0f, std::max(std::fabs(fa), std::fabs(fb)));
}
}  // namespace

void Compare(const uint8_t* gpu, const uint8_t* cpu, uint32_t vertices, uint32_t kernel, uint32_t physical) {
  static std::atomic<int> said{0};
  uint64_t same = 0, near = 0, far = 0;
  for (uint32_t v = 0; v < vertices; ++v) {
    uint32_t g[8], c[8];
    std::memcpy(g, gpu + size_t(v) * 32, 32);
    std::memcpy(c, cpu + size_t(v) * 32, 32);
    for (int w = 0; w < 8; ++w) {
      if (g[w] == c[w]) {
        ++same;
        continue;
      }
      const bool close = w < 3 ? FloatNear(g[w], c[w]) : w >= 6 ? PackedNear(g[w], c[w]) : false;
      if (close) {
        ++near;
        continue;
      }
      ++far;
      if (said.fetch_add(1, std::memory_order_relaxed) < 20) {
        REXLOG_WARN("[gpu_skin] check: kernel {} vertex {} of {:08X} word {}: GPU {:08X}, CPU {:08X}", kernel + 1, v,
                    physical, w, g[w], c[w]);
      }
    }
  }
  g_counts.checked_vertices.fetch_add(vertices, std::memory_order_relaxed);
  g_counts.words_same.fetch_add(same, std::memory_order_relaxed);
  g_counts.words_near.fetch_add(near, std::memory_order_relaxed);
  g_counts.words_far.fetch_add(far, std::memory_order_relaxed);
}

void Report() {
  if (!Enabled()) {
    return;
  }
  const auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
  const uint64_t recorded = take(g_recorded), vertices = take(g_recorded_vertices), forgotten = take(g_forgotten),
                 not_recorded = take(g_not_recorded), bone_bytes = take(g_bone_bytes);
  const int64_t live = g_live.load(std::memory_order_relaxed);
  const uint64_t made = g_jobs_made.load(std::memory_order_relaxed);
  RendererCounts& c = g_counts;
  const uint64_t ranges = take(c.ranges), dispatches = take(c.dispatches), skinned = take(c.vertices),
                 gaps = take(c.gap_bytes), refused = take(c.refused), refused_vertices = take(c.refused_vertices),
                 inputs = take(c.input_bytes), ns = take(c.ns), hits = take(c.cache_hits),
                 fills = take(c.cache_fills), fill_bytes = take(c.cache_fill_bytes), uncached = take(c.uncached),
                 guard_fails = take(c.guard_fails), submit_dispatches = take(c.submit_dispatches);
  REXLOG_INFO("[gpu_skin] last 10 s: {} calls recorded ({} vertices, {:.1f} KB of bone snapshots), {} not recorded "
              "(skinned on the CPU), {} forgotten, {} jobs held ({} made since start); renderer: {} ranges, {} job "
              "parts in {} dispatches, {} vertices, {:.1f} MB copied per use, model inputs {} cached / {} filled "
              "({:.1f} MB) / {} uncached, "
              "{} guard failures, {:.1f} MB of gaps copied, {} ranges refused ({} vertices), {:.2f} ms of ring time",
              recorded, vertices, double(bone_bytes) / 1e3, not_recorded, forgotten, live, made, ranges, dispatches,
              submit_dispatches, skinned, double(inputs) / 1e6, hits, fills, double(fill_bytes) / 1e6, uncached,
              guard_fails, double(gaps) / 1e6, refused, refused_vertices, double(ns) / 1e6);
  const uint64_t checked = take(c.checked_vertices);
  if (checked) {
    REXLOG_INFO("[gpu_skin] check, last 10 s: {} vertices compared with the CPU kernels: {} words identical, {} near "
                "(a float within 1e-5 relative, a packed field by one), {} different",
                checked, take(c.words_same), take(c.words_near), take(c.words_far));
  }
}

}  // namespace bo2zm::gpu_skin
