// GPU skinning (bo2zm_gpu_skin): the game's CPU vertex skinning moved to a compute shader.
//
// The game skins its animated models on the CPU: sub_82914AA0 runs one kernel per group of a surface's vertices
// (one, two, three and four bones each; r7 + 32 bytes per vertex, the groups one after the other), and the draws then
// read that output as a vertex buffer (stride 32). Even rewritten natively (bo2zm_native_skin.cpp), the three main
// kernels cost about 11 % of a core at round 15. With GPU skinning on, their hooks record a job instead of computing:
// the call's arguments, a snapshot of the bone matrices it reads (they may be gone by the time the frame is drawn) and
// the constants it reads at that moment. The guest output is left unwritten. When the renderer copies a vertex range
// that holds a job's output, it copies the job's inputs into the upload buffer instead and records a compute dispatch
// (shaders/bo2zm_gpu_skin.comp) that writes the skinned vertices where the copy would have put them; the parts of the
// range that hold no job (the four-bone group, which stays on the CPU) are copied as usual.
//
// Which job owns a range: the newest one recorded for it. Only the skinning kernels write the skinned output pages (a
// write watch on the PC confirmed this), and the hook on sub_82914AA0 forgets every job inside the surface's whole
// output before its kernels run, so a range that a kernel without a hook writes later (the four-bone one, or a
// variant) is never served from an older job. The inputs (the model's vertices and bone records) are read when the
// frame is drawn; we believe they are static model data (the fastfile's surfaces).

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace bo2zm::gpu_skin {

struct Job {
  uint32_t out = 0;     // physical address of the output (the kernel's r7 rounded down to 16, as its stvx)
  uint32_t count = 0;   // vertices
  uint32_t kernel = 0;  // 0, 1, 2: one, two, three bones
  const uint8_t* vertices = nullptr;  // the input vertices (24 bytes each), host pointer into guest memory
  uint32_t vertices_guest = 0;        // their guest address
  const uint8_t* records = nullptr;   // the bone records (u16: 1, 3 or 5 per vertex), host pointer
  uint8_t* target = nullptr;          // the output in guest memory (host pointer), for Materialize
  uint32_t records_guest = 0;         // their guest address (where their words start)
  uint32_t bones_guest = 0;           // the kernel's r6
  uint32_t bones_lo = 0;              // guest address of bones[0]
  // Eight vectors as values (host order): position scale, bias, K8, K9, K10, K11, one, the weight lanes.
  uint32_t constants[32] = {};
  std::vector<uint8_t> bones;  // the matrices' bytes as they were in guest memory, from bones_lo
  bool cpu_written = false;    // bo2zm_gpu_skin_check: the kernel ran too, its output is in guest memory
  uint64_t serial = 0;
  // Set once when a newer job or the dispatcher's hook takes its output, or Materialize computed it: the store's
  // entries of a forgotten job are skipped and dropped as they are met.
  mutable std::atomic<bool> forgotten{false};
  std::atomic<uint32_t> refs{0};  // JobRef
};

// A counted reference to a job. Jobs come from a pool and return to it when the last reference goes (bones keeps its
// capacity), so recording a call allocates nothing once the pool has grown to the number in use. Two allocations per
// call, and the frees behind them, had made the C heap's lock the job workers' next point of contention.
class JobRef {
 public:
  JobRef() = default;
  JobRef(const JobRef& other) : job_(other.job_) { Retain(); }
  JobRef(JobRef&& other) noexcept : job_(other.job_) { other.job_ = nullptr; }
  JobRef& operator=(const JobRef& other) {
    if (this != &other) {
      Release();
      job_ = other.job_;
      Retain();
    }
    return *this;
  }
  JobRef& operator=(JobRef&& other) noexcept {
    if (this != &other) {
      Release();
      job_ = other.job_;
      other.job_ = nullptr;
    }
    return *this;
  }
  ~JobRef() { Release(); }
  Job* operator->() const { return job_; }
  Job& operator*() const { return *job_; }
  Job* get() const { return job_; }
  explicit operator bool() const { return job_ != nullptr; }
  bool operator==(const JobRef& other) const { return job_ == other.job_; }
  // A job taken from the pool (or made), reset, with this reference only.
  static JobRef New();

 private:
  void Retain() {
    if (job_) {
      job_->refs.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void Release();
  Job* job_ = nullptr;
};

// --- The hooks --------------------------------------------------------------------------------------------------

// bo2zm_gpu_skin or its A/B factor is set: the dispatcher's hook forgets the jobs in each surface's output.
bool Enabled();
// This kernel call: record a job instead of computing (bo2zm_gpu_skin, or its arm of the A/B).
bool WantedNow();
// bo2zm_gpu_skin_check: the next job also runs the CPU kernel, and the renderer compares the two outputs.
bool CheckNext();
// The physical address of a guest one, for the views of physical memory (0xA0000000 and up) only.
bool Physical(uint32_t guest, uint32_t& physical);
// sub_82914AA0's hook: jobs with output in [guest, guest + bytes) are forgotten.
void Forget(uint32_t guest, uint32_t bytes);
// A recorded call (out already physical). Forgets the jobs it overlaps.
void Add(JobRef job);
// The kernels' calls that could not become jobs (unaligned vertices, the general form's vperm controls, ...).
void CountNotRecorded();

// --- The renderer (ring thread) ----------------------------------------------------------------------------------

// Whether the renderer should take jobs (the ring thread reads this once at start).
bool RendererOn();
// The renderer's compute pipeline exists: until then (and if it never does) the hooks record nothing.
void SetRendererReady(bool ready);
// Cheap: some job was ever recorded in a 64 KB region of [physical, physical + bytes).
bool MayOverlap(uint32_t physical, uint32_t bytes);
// The jobs whose output overlaps [physical, physical + bytes), by address (they never overlap each other).
void Overlapping(uint32_t physical, uint32_t bytes, std::vector<JobRef>& jobs);

// A range the GPU path cannot take: the job's output computed on the CPU (from its snapshot, the shader's
// operations) into guest memory, where the vertex copy reads it, and the job dropped.
void Materialize(const Job& job);

// What the renderer did, for the 10 s report.
struct RendererCounts {
  std::atomic<uint64_t> ranges{0};       // vertex ranges that held jobs
  std::atomic<uint64_t> dispatches{0};   // compute dispatches (one per job and range)
  std::atomic<uint64_t> vertices{0};     // vertices skinned by them
  std::atomic<uint64_t> gap_bytes{0};    // bytes of those ranges copied as usual (no job)
  std::atomic<uint64_t> refused{0};      // ranges that held a job but could not take the GPU path (see the reason)
  std::atomic<uint64_t> refused_vertices{0};
  std::atomic<uint64_t> input_bytes{0};  // bytes copied into the upload buffer per use (constants, bones, uncached inputs)
  std::atomic<uint64_t> cache_hits{0};   // job parts whose model inputs were in the input cache
  std::atomic<uint64_t> cache_fills{0};  // ... copied into it now
  std::atomic<uint64_t> cache_fill_bytes{0};
  std::atomic<uint64_t> uncached{0};     // ... copied into the upload buffer (no room, or not in physical memory)
  std::atomic<uint64_t> guard_fails{0};  // cached inputs whose guest bytes changed without a write stamp
  std::atomic<uint64_t> submit_dispatches{0};  // compute dispatches (one per submission, more if a table fills)
  std::atomic<uint64_t> ns{0};           // ring thread time in the GPU path
  // bo2zm_gpu_skin_check, compared when the upload buffer comes back from the GPU.
  std::atomic<uint64_t> checked_vertices{0};
  std::atomic<uint64_t> words_same{0};
  std::atomic<uint64_t> words_near{0};  // a float within 1e-5 relative, or a packed field by one
  std::atomic<uint64_t> words_far{0};
};
RendererCounts& Counts();
// Logs why a range was refused (the first few times per reason).
void Refused(const char* reason, uint32_t physical, uint32_t bytes, uint32_t stride, uint32_t order);

// The CPU's and the GPU's 32-byte vertices (as the vertex copy leaves them: k8in32 values), compared word by word.
void Compare(const uint8_t* gpu, const uint8_t* cpu, uint32_t vertices, uint32_t kernel, uint32_t physical);

// A "[gpu_skin]" line every 10 s (from bo2zm_native_audio.cpp's summary).
void Report();

}  // namespace bo2zm::gpu_skin
