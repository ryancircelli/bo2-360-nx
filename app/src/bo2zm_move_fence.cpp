// The streaming pool's block moves and the fence that says they are done (bo2zm_move_fence_fix), with an audit.
//
// What the game does
// ------------------
// The streaming pool moves texture blocks with the GPU. A move is queued (sub_82901078, flag bit 0) in a table T at
// 0x84BE6240: 16 bytes per entry {destination, source, size, flags}, then
//   T+0x8000 entries queued          T+0x8004 entries to issue (set when a generation is closed)
//   T+0x8008 the open generation     T+0x800C the last closed generation
//   T+0x8010 the generation issued   T+0x8014 the generation completed
//   T+0x8018 the fence after the last issued batch                T+0x801C "that fence is still to be tested"
// Once per frame sub_829014F8 closes the open generation. The render side (sub_82901218, in critical section 7) issues
// the closed entries as GPU copies, inserts a D3D fence and then stores, in this order: T+0x8018 = the fence,
// T+0x801C = 1, T+0x8010 = the closed generation. Memory the pool hands out is tied to the generation that was open
// when it was allocated (sub_828F2398 stores it in the request slot), and sub_828EFB10 gives the address to the
// loader only when sub_82901438(generation) says that generation completed: every move queued before the allocation,
// including the one out of the block being reused, has then run on the GPU.
//
// The flaw
// --------
// sub_82901438 takes no lock. It reads T+0x8018, asks D3D whether that fence is pending (sub_82161DC0), and if it is
// not, only then reads T+0x8010 and stores it as the completed generation (and clears T+0x801C). If the render side
// publishes a new batch between those two reads, the fence that was tested is the previous batch's while the
// generation adopted is the new batch's: the new generation counts as completed while its copies sit in a command
// buffer that has not even been kicked. The pool then hands a moved block's old place to the loader. On the console
// the loader's disc read takes far longer than the rest of the frame, so the copy has run by the time the data
// arrives; here a read completes at once, the loader refills the block, and the GPU copy that runs afterwards moves
// the new texture's bytes into the other texture: blocky garbage in a world texture, for as long as it stays loaded.
//
// The fix
// -------
// sub_82901438 natively, the same steps except that the issued generation is read before the fence (the fence read
// after it can only be the same batch's or a later one's, so the generation adopted is never newer than the fence
// tested), and the stores are made only while the render side is not publishing (a host lock the render side holds
// across sub_82901218; this side only tries it and answers "not yet" when it is taken).
//
// The audit (always on, both ways)
// --------------------------------
//   PREMATURE: the completed generation advanced to a generation whose fence is still pending.
//   Hand-outs (sub_828EFB10 returning a block) that overlap the source or the destination of a move that is still in
//   the table (not issued) or was issued in a batch whose fence is still pending.
// "[move-fence]" lines log each of the first events and a summary every 10 s.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bo2zm_move_fence_fix, true, "BO2",
                    "The streaming pool's \"move generation completed\" test (sub_82901438) in native code, so a "
                    "generation is never adopted with the previous batch's fence (bo2zm_move_fence.cpp); false = the "
                    "game's code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82901438);
REX_EXTERN(__imp__sub_82901218);
REX_EXTERN(__imp__sub_828EFB10);
REX_EXTERN(__imp__sub_82161DC0);

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTable = 0x84BE6240;
constexpr uint32_t kQueued = kTable + 0x8000;
constexpr uint32_t kToIssue = kTable + 0x8004;
constexpr uint32_t kIssued = kTable + 0x8010;
constexpr uint32_t kCompleted = kTable + 0x8014;
constexpr uint32_t kFence = kTable + 0x8018;
constexpr uint32_t kFenceToTest = kTable + 0x801C;
constexpr int32_t kMaxEntries = 0x800;

// The allocation request slots (sub_828F2398, sub_828EFB10): 20 bytes each, state +0, size +8, address +12,
// generation +16.
constexpr uint32_t kSlots = 0x84BCE400;
constexpr int32_t kSlotCount = 256;

// sub_82161DC0: the device is *[0x82000A74] (0x82000A70 for the system process); the last fence inserted is at
// device+11036 and device+11024 points to the last fence the GPU passed.
constexpr uint32_t kDevicePointer = 0x82000A74;
constexpr uint32_t kDevicePointerSystem = 0x82000A70;
constexpr uint32_t kReturnFromPendingTest = 0x82901498;

constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;
constexpr uint64_t kLogged = 32;

inline uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
inline void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

struct Move {
  uint32_t destination;
  uint32_t source;
  uint32_t size;
};
struct Batch {
  uint64_t id = 0;
  bool issued = false;  // false while sub_82901218 is still issuing it
  uint32_t fence = 0;
  uint32_t generation = 0;
  std::vector<Move> moves;
};

bool g_fix = true;
std::mutex g_publishing;  // held by the render side across sub_82901218
std::mutex g_batches_lock;
std::deque<Batch> g_batches;
uint64_t g_batch_id = 0;

std::atomic<uint64_t> g_polls{0};
std::atomic<uint64_t> g_adoptions{0};
std::atomic<uint64_t> g_premature{0};
std::atomic<uint64_t> g_batches_issued{0};
std::atomic<uint64_t> g_moves_issued{0};
std::atomic<uint64_t> g_handouts{0};
std::atomic<uint64_t> g_over_queued_source{0};
std::atomic<uint64_t> g_over_queued_destination{0};
std::atomic<uint64_t> g_over_issued_source{0};
std::atomic<uint64_t> g_over_issued_destination{0};
std::atomic<uint64_t> g_table_busy{0};
std::atomic<uint64_t> g_fix_overtaken{0};         // a new batch was published during the fence test
std::atomic<uint64_t> g_fix_would_premature{0};   // ... and the game's code would have adopted it, fence pending
std::atomic<uint64_t> g_fix_publishing{0};        // the stores were skipped: the render side was publishing
std::atomic<int64_t> g_last_report_ns{0};

void Init() {
  static const bool once = [] {
    g_fix = REXCVAR_GET(bo2zm_move_fence_fix);
    REXLOG_INFO("[move-fence] fix {}", g_fix ? "on" : "off");
    return true;
  }();
  (void)once;
}

// sub_82161DC0 without its kick.
bool FencePending(const uint8_t* base, uint32_t fence, uint32_t* last_out = nullptr, uint32_t* passed_out = nullptr) {
  if (fence == 0) {
    return false;
  }
  uint32_t p = Read32(base, kDevicePointer);
  if (p == 0) {
    p = Read32(base, kDevicePointerSystem);
  }
  if (p == 0) {
    return false;
  }
  const uint32_t device = Read32(base, p);
  if (device == 0) {
    return false;
  }
  const uint32_t passed_pointer = Read32(base, device + 11024);
  if (passed_pointer == 0) {
    return false;
  }
  const uint32_t last = Read32(base, device + 11036);
  const uint32_t passed = Read32(base, passed_pointer);
  if (last_out) {
    *last_out = last;
  }
  if (passed_out) {
    *passed_out = passed;
  }
  return uint32_t(last - fence) < uint32_t(last - passed);
}

inline bool Overlaps(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
  a &= kPhysicalMask;
  b &= kPhysicalMask;
  return a < b + b_size && b < a + a_size;
}

void Report(const uint8_t* base) {
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  int64_t last = g_last_report_ns.load(std::memory_order_relaxed);
  if (now - last < 10'000'000'000LL || !g_last_report_ns.compare_exchange_strong(last, now)) {
    return;
  }
  (void)base;
  REXLOG_INFO("[move-fence] summary (fix {}): polls {}, adoptions {}, PREMATURE {}; batches {}, moves {}; hand-outs {}, "
              "over a move not issued: source {} destination {}, over a move issued with its fence pending: source {} "
              "destination {} (table scans skipped {}); fix: batch published during the test {}, the game's code "
              "would have adopted it early {}, stores skipped {}",
              g_fix ? "on" : "off", g_polls.load(), g_adoptions.load(), g_premature.load(), g_batches_issued.load(),
              g_moves_issued.load(), g_handouts.load(), g_over_queued_source.load(), g_over_queued_destination.load(),
              g_over_issued_source.load(), g_over_issued_destination.load(), g_table_busy.load(),
              g_fix_overtaken.load(), g_fix_would_premature.load(), g_fix_publishing.load());
}

// Batches whose fence the GPU passed are done. Caller holds g_batches_lock.
void Prune(const uint8_t* base) {
  while (!g_batches.empty() && g_batches.front().issued && !FencePending(base, g_batches.front().fence)) {
    g_batches.pop_front();
  }
  while (g_batches.size() > 256) {
    g_batches.pop_front();
  }
}

void Handout(const uint8_t* base, uint32_t address, uint32_t size, uint32_t generation, uint32_t thread) {
  g_handouts.fetch_add(1, std::memory_order_relaxed);
  // Moves still in the table.
  if (g_publishing.try_lock()) {
    int32_t queued = int32_t(Read32(base, kQueued));
    if (queued > kMaxEntries) {
      queued = kMaxEntries;
    }
    for (int32_t i = 0; i < queued; ++i) {
      const uint32_t e = kTable + uint32_t(i) * 16;
      const Move m{Read32(base, e), Read32(base, e + 4), Read32(base, e + 8)};
      const bool source = Overlaps(address, size, m.source, m.size);
      const bool destination = Overlaps(address, size, m.destination, m.size);
      if (!source && !destination) {
        continue;
      }
      const uint64_t n = (source ? g_over_queued_source : g_over_queued_destination).fetch_add(1) + 1;
      if (n <= kLogged) {
        REXLOG_WARN("[move-fence] HAND-OUT {:08X}+{:X} (generation {}, completed {}, issued {}) over the {} of a move "
                    "not issued yet: {:08X} <- {:08X} +{:X} (entry {} of {}), thread {:08X}",
                    address, size, generation, Read32(base, kCompleted), Read32(base, kIssued),
                    source ? "source" : "destination", m.destination, m.source, m.size, i, queued, thread);
      }
    }
    g_publishing.unlock();
  } else {
    g_table_busy.fetch_add(1, std::memory_order_relaxed);
  }
  // Moves issued (or being issued) whose fence the GPU has not passed.
  std::lock_guard<std::mutex> lock(g_batches_lock);
  Prune(base);
  for (const Batch& b : g_batches) {
    uint32_t last = 0;
    uint32_t passed = 0;
    if (b.issued && !FencePending(base, b.fence, &last, &passed)) {
      continue;
    }
    for (const Move& m : b.moves) {
      const bool source = Overlaps(address, size, m.source, m.size);
      const bool destination = Overlaps(address, size, m.destination, m.size);
      if (!source && !destination) {
        continue;
      }
      const uint64_t n = (source ? g_over_issued_source : g_over_issued_destination).fetch_add(1) + 1;
      if (n <= kLogged) {
        REXLOG_WARN("[move-fence] HAND-OUT {:08X}+{:X} (generation {}, completed {}, issued {}) over the {} of a move "
                    "whose fence is pending: {:08X} <- {:08X} +{:X}, batch generation {} fence {:08X}{} (last "
                    "inserted {:08X}, passed {:08X}), thread {:08X}",
                    address, size, generation, Read32(base, kCompleted), Read32(base, kIssued),
                    source ? "source" : "destination", m.destination, m.source, m.size, b.generation, b.fence,
                    b.issued ? "" : " (still being issued)", last, passed, thread);
      }
    }
  }
}

}  // namespace

// The render side issues the closed moves and publishes their fence and generation.
REX_HOOK_RAW(sub_82901218) {
  Init();
  int32_t count = int32_t(Read32(base, kToIssue));
  if (count > kMaxEntries) {
    count = kMaxEntries;
  }
  uint64_t id = 0;
  if (count > 0) {
    Batch b;
    b.moves.reserve(size_t(count));
    for (int32_t i = 0; i < count; ++i) {
      const uint32_t e = kTable + uint32_t(i) * 16;
      b.moves.push_back(Move{Read32(base, e), Read32(base, e + 4), Read32(base, e + 8)});
    }
    std::lock_guard<std::mutex> lock(g_batches_lock);
    id = b.id = ++g_batch_id;
    g_batches.push_back(std::move(b));
  }
  {
    std::lock_guard<std::mutex> publishing(g_publishing);
    __imp__sub_82901218(ctx, base);
  }
  if (id != 0) {
    std::lock_guard<std::mutex> lock(g_batches_lock);
    for (Batch& b : g_batches) {
      if (b.id == id) {
        b.fence = Read32(base, kFence);
        b.generation = Read32(base, kIssued);
        b.issued = true;
        break;
      }
    }
    g_batches_issued.fetch_add(1, std::memory_order_relaxed);
    g_moves_issued.fetch_add(uint64_t(count), std::memory_order_relaxed);
    Prune(base);
  }
  Report(base);
}

// "Has this move generation completed?"
REX_HOOK_RAW(sub_82901438) {
  Init();
  g_polls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t generation = ctx.r3.u32;
  const uint32_t completed_before = Read32(base, kCompleted);
  if (!g_fix) {
    __imp__sub_82901438(ctx, base);
  } else {
    uint32_t completed = completed_before;
    if (generation > completed && Read32(base, kFenceToTest) != 0) {
      const uint32_t issued = Read32(base, kIssued);  // before the fence: never newer than the fence tested
      const uint32_t fence = Read32(base, kFence);
      const uint64_t lr = ctx.lr;
      ctx.r3.u64 = fence;
      ctx.lr = kReturnFromPendingTest;
      __imp__sub_82161DC0(ctx, base);
      ctx.lr = lr;
      if (ctx.r3.u32 == 0) {
        if (Read32(base, kIssued) != issued) {
          g_fix_overtaken.fetch_add(1, std::memory_order_relaxed);
          if (FencePending(base, Read32(base, kFence))) {
            g_fix_would_premature.fetch_add(1, std::memory_order_relaxed);
          }
        }
        if (g_publishing.try_lock()) {
          if (issued > Read32(base, kCompleted)) {
            Write32(base, kCompleted, issued);
          }
          if (Read32(base, kFence) == fence && Read32(base, kIssued) == issued) {
            Write32(base, kFenceToTest, 0);
          }
          g_publishing.unlock();
        } else {
          g_fix_publishing.fetch_add(1, std::memory_order_relaxed);
        }
      }
      completed = Read32(base, kCompleted);
    }
    ctx.r3.u64 = generation <= completed ? 1 : 0;
  }
  // The audit: a generation adopted while its fence is pending.
  const uint32_t completed_after = Read32(base, kCompleted);
  if (completed_after != completed_before) {
    g_adoptions.fetch_add(1, std::memory_order_relaxed);
    const uint32_t issued_a = Read32(base, kIssued);
    const uint32_t fence = Read32(base, kFence);
    const uint32_t issued_b = Read32(base, kIssued);
    uint32_t last = 0;
    uint32_t passed = 0;
    if (issued_a == completed_after && issued_b == completed_after && FencePending(base, fence, &last, &passed)) {
      const uint64_t n = g_premature.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= kLogged) {
        REXLOG_WARN("[move-fence] PREMATURE #{}: generation {} adopted as completed (was {}) while its fence {:08X} "
                    "is pending (last inserted {:08X}, passed {:08X}); asked for generation {}, thread {:08X}",
                    n, completed_after, completed_before, fence, last, passed, generation, ctx.r13.u32);
      }
    }
  }
  Report(base);
}

// The pool gives an allocated block to its requester (r3 = the request slot, r4 = where the address goes).
REX_HOOK_RAW(sub_828EFB10) {
  const int32_t index = int32_t(ctx.r3.u32);
  uint32_t address = 0;
  uint32_t size = 0;
  uint32_t generation = 0;
  if (index >= 0 && index < kSlotCount) {
    const uint32_t slot = kSlots + uint32_t(index) * 20;
    size = Read32(base, slot + 8);
    address = Read32(base, slot + 12);
    generation = Read32(base, slot + 16);
  }
  __imp__sub_828EFB10(ctx, base);
  if ((ctx.r3.u32 & 0xFF) != 0 && address != 0 && size != 0) {
    Handout(base, address, size, generation, ctx.r13.u32);
  }
}
