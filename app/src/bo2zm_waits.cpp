// Blocking waits for the game's job system. The map of the job system (queues, job records, events) is in
// bo2zm_jobs.h.
//
// WHY
// BO2's threads never block while they wait for something in its job system: on the Xbox 360 each one had a hardware
// thread to itself. On the Switch's three cores a waiting thread takes time from the ones with work, and on the PC it
// costs a whole core. Measured on the PC (Survival: Town, 60 FPS, bo2zm_jobs_report_s): the Backend spends 99 % of its
// time in its "wait for the GPU to drain the ring" loop and Main 96 % in its "wait for the Backend to finish the frame"
// loop, each at ~98 % of a core. On the Switch the Backend spent 28 % of its samples yielding in its frame-event poll,
// and the job workers polled the job event every millisecond.
//
// THE GAME'S WAITS, all in the same few shapes:
//   1. Help loops, sub_824935E8 / sub_82493548(poll, flag): until poll(0) says done, run one job at a time
//      (sub_8284BB48) when any job is queued. The polls: sub_82428ED8 (the Backend's frame event), sub_82428840
//      (Main's "Backend done" event), sub_828DA6F0 (the GPU ring drained).
//   2. The workers' wait, sub_8284A088(slot): waits on the job event in 1 ms slices while nothing is queued, and
//      only yields when jobs are queued that this worker cannot take (in queues waiting for release).
//   3. "Wait for a job type", sub_8284B730(counters, limit): helps from slot 0, yields and spins while the jobs it
//      waits for run elsewhere. sub_82493690 is the same around sub_8284BB48.
//   3b. The Server waits for its client jobs the same way (sub_8243BFD0: help once until each job's done flag).
//   4. Jobs with a dependency (record +20 counter, +24 target) are put back without running until the counter
//      reaches the target, and picked again at once: whoever picks one spins through the queue.
//   5. Jobs that need the game's critical section 6 (index 6 of its table at 0x83DE0218: sub_828D1B30 and
//      sub_828F2768 -> sub_828F2398, the job types 0x82A99270 and 0x82A99D98) only try it (sub_828EFC68 ->
//      sub_824A0B70(6)) and ask to be run again when another thread holds it: on the PC ~30,000 such put-backs per
//      second, the workers going round the queue until the lock is free.
//   6. Jobs that wait for other job types by hand: they return "again" while jobs of the types they wait for are
//      pending (sub_828DFAB8 through sub_828DF118 waits for types 0x82A99AE8, 0x82A99B10 and 0x82A99B60; the same
//      for sub_828DFBB0, sub_828DFD08 and others), and are put back. Together with the section 6 jobs, on the PC Main
//      put back 6,000-12,000 jobs a second while it waited for the Backend, the Backend 4,500-16,000 while it waited
//      for the ring, and each worker 3,500-8,000 (on the benchmark route, varying from one run to the next).
//   7. One of those, sub_828DF468 (type 0x82A99868), waits for the culling jobs (type 0x82A99A20, sub_828DF838): its
//      sub_828DED00 returns "again" while their pending count, the 64-bit word at 0x82A99A30 (sub_82849F58), is not
//      zero. On the Switch at round 15 Main put it back 12,900-14,700 times a second.
// The picker sub_8284AED8 yields (NtYieldExecution) each time it finds nothing.
//
// WHAT THE HOOKS DO
// A thread that has nothing it may run sleeps on a host condition until something it waits for may have changed:
//   - a job pushed (sub_8284A910) or a released queue's jobs moved (sub_8284AB88); a queue put on the release list
//     (sub_8284AB18, from a GPU callback), so that a pick releases it;
//   - a job finished: the thread that ran it notifies at its next pick or when it leaves its loop;
//   - the Backend's events set (sub_82428878 and sub_82429748 set the frame event, sub_82429708 "frame done");
//   - the native renderer read further into the ring (WarnProgressRing).
// The change counters are read before the game's own check, so a change between the check and the sleep is never
// lost. Every sleep is bounded (bo2zm_waits_max_us; four times that for an idle worker), and afterwards the game's
// own function runs as before and decides: nothing of the game is skipped, only its spinning is replaced by sleep.
// A thread never sleeps while it holds the D3D device lock (sub_824291B0): in the help loops it sleeps right after
// releasing it (sub_828DB270), so threads waiting for the device are not held up.
//
// The shapes that put jobs back need a little more:
//   - A thread that picks a job whose dependency is not met (4) holds it and sleeps for the game's own pause between
//     two looks at the counter (50,000 timebase ticks), or until a job is pushed.
//   - A job worker whose tries of critical section 6 failed kMissesSection times in a row (5) sleeps until the
//     section is left (sub_824A0BC0(6) notifies) or a job is pushed, then tries once more: when it gets the section
//     the job runs instead of being put back; when not, the job is put back as before. The try itself is unchanged.
//     "Something else queued" cannot be the test, because what is queued is mostly the other jobs that need the same
//     section: on the PC the section is left ~81 times a second while ~33,000 tries a second fail, and a check on
//     queued jobs let the workers sleep only 9 times a second.
//   - A JQ worker, or Main or the Backend inside its help loop, whose last kPutBacksRow jobs were all put back (6; any
//     job that runs to its end starts the count again) sleeps until a job ends, a job is pushed or section 6 is left
//     (Main and the Backend also until their loop's own wait may be over: the Backend's events, ring progress). A
//     put-back job no longer counts as ended for the threads that wait for its type. With this sleep the put-backs on
//     the PC fell to 8,100 a second for Main, 2,300-4,200 for the Backend and 1,600-3,200 for each worker, with frame
//     times unchanged.
//   - The thread that picks the wait for the culling jobs (7) sleeps in sub_828DED00 until a culling job ends (a
//     job's end is also announced with its type's key, besides its group's) or the bound, then the check runs as
//     before.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/swap_hook.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "bo2zm_benchmark.h"
#include "bo2zm_jobs.h"
#include "native/render/bo2zm_native_render.h"

// On by default: in three interleaved A/B pairs on the PC, frame times were the same and the process used 1.3-1.7
// cores less. That comparison was made on the PC only. Off keeps the older, simpler Backend sleep on the Switch (in
// sub_82428ED8's hook below).
REXCVAR_DEFINE_BOOL(bo2zm_blocking_waits, true, "BO2",
                    "Game threads with nothing to run in the job system sleep until a job, a job's end, the "
                    "Backend's events or ring progress may let them go on, instead of yielding in a loop. "
                    "false = the game's own spinning (and, on the Switch, bd67688's short Backend sleep)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_waits_max_us, 2000, "BO2",
                     "Longest sleep of a blocking job-system wait, in microseconds (an idle worker sleeps up to four "
                     "times this); afterwards the game checks again")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// See "Sleepers with a lock of their own" below.
REXCVAR_DEFINE_BOOL(bo2zm_waits_own_locks, true, "BO2",
                    "Blocking job-system waits: each sleeping thread waits on a lock and condition of its own, and a "
                    "notifier takes only the locks of the threads it wakes. false = one lock for every sleeper and "
                    "notifier, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_waits_own_locks_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_waits_own_locks for the sleeps that start "
                    "in each arm (on = a lock of their own)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Shape 7 in the header comment.
REXCVAR_DEFINE_BOOL(bo2zm_waits_cull, true, "BO2",
                    "Blocking job-system waits: a job that waits for the culling jobs (sub_828DED00) sleeps until one "
                    "of them ends instead of being put back and picked again at once. false = the game's own spinning")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_waits_cull_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_waits_cull. 0 = not part "
                     "of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(bo2zm_wait_sites, 0xFFF, "BO2",
                     "Which blocking waits are on, one bit each (diagnostics): 1 frame poll, 2 done poll, 4 ring "
                     "poll, 8 after the device lock is released, 16 workers, 32 type waits, 64 group waits, "
                     "128 the Server, 256 jobs whose dependency is not met, 512 workers that find critical "
                     "section 6 taken, 1024 threads whose last jobs were all put back, 2048 the wait for the culling "
                     "jobs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_waits_fence_wake, true, "BO2",
                    "Wake threads sleeping on put-back jobs when the SDK command processor writes a fence "
                    "(EVENT_WRITE_SHD), and let JQ workers wait for that too. The three put-back job types wait for a "
                    "D3D fence (sub_828AA590), which only the command processor completes; without this they wake on "
                    "the bo2zm_waits_max_us timeout")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8284AED8);
REX_EXTERN(__imp__sub_827D8DD8);
REX_EXTERN(__imp__sub_8284A088);
REX_EXTERN(__imp__sub_8284A910);
REX_EXTERN(__imp__sub_8284AB18);
REX_EXTERN(__imp__sub_8284AB88);
REX_EXTERN(__imp__sub_8284BB48);
REX_EXTERN(__imp__sub_8284B730);
REX_EXTERN(__imp__sub_82493190);
REX_EXTERN(__imp__sub_827D4BD8);
REX_EXTERN(__imp__sub_82493690);
REX_EXTERN(__imp__sub_82493240);
REX_EXTERN(__imp__sub_824931E8);
REX_EXTERN(__imp__sub_82493548);
REX_EXTERN(__imp__sub_824935E8);
REX_EXTERN(__imp__sub_82428ED8);
REX_EXTERN(__imp__sub_82428840);
REX_EXTERN(__imp__sub_828DA6F0);
REX_EXTERN(__imp__sub_828DB270);
REX_EXTERN(__imp__sub_8284C3B8);
REX_EXTERN(__imp__sub_82428878);
REX_EXTERN(__imp__sub_82429748);
REX_EXTERN(__imp__sub_82429708);
REX_EXTERN(__imp__sub_828EFC68);
REX_EXTERN(__imp__sub_824A0BC0);

namespace tr = bo2zm::jobs;

#if REX_PLATFORM_SWITCH
// The same prototypes as libnx's svc.h (switch.h is not included here: its Thread/Event names clash).
extern "C" uint32_t svcWaitForAddress(void* address, uint32_t arb_type, int64_t value, int64_t timeout);
extern "C" uint32_t svcSignalToAddress(void* address, uint32_t signal_type, int32_t value, int32_t count);
#endif

namespace {

// --- The D3D device lock (sub_824291B0 takes it, sub_82429358 gives it back) ------------------------------------
constexpr uint32_t kDeviceOwner = BO2_GAME_ADDR(0x83AF72E4, 0x8372AB20);  // thread id of the owner (stored by sub_824291B0)

// Return addresses inside the help loops (see the loops in the header comment).
constexpr uint32_t kReleaseInLoop1 = BO2_GAME_ADDR(0x82493630, 0x823CA838);   // sub_828DB270 from sub_824935E8
constexpr uint32_t kReleaseInLoop2 = BO2_GAME_ADDR(0x824935A0, 0x823CA7A8);   // sub_828DB270 from sub_82493548
constexpr uint32_t kHelpInGroup1 = BO2_GAME_ADDR(0x824936EC, 0x823CA8F4);    // sub_8284BB48 from sub_82493690
constexpr uint32_t kHelpInGroup2 = BO2_GAME_ADDR(0x82493754, 0x823CA95C);
// The Server's wait for its client jobs (sub_8243BFD0 at 0x8243C500): per client, until the job sets the done flag
// at +0x5140, help once with that job type (sub_82493240) or with any job (sub_824931E8, returning to 0x8243C564).
// The jobs: definition 0x82982790 -> type descriptor 0x829827E0 (function sub_824384E8, which stores the flag);
// their key is the descriptor + 16.
constexpr uint32_t kHelpServer = BO2_GAME_ADDR(0x8243C564, 0);  // the campaign has no such wait (its group wait)
constexpr uint32_t kKeyServer = BO2_GAME_ADDR(0x829827E0, 0x82751250) + 16;

// How many failed tries of critical section 6 in a row (case 5 in the header comment), and how many put-back jobs in
// a row (case 6), make a thread sleep.
constexpr int kMissesSection = 3;
constexpr int kPutBacksRow = 4;
// Where the game's job loops put a job back right after running it (sub_8284C3B8 returns there).
constexpr uint32_t kPutBack1 = BO2_GAME_ADDR(0x8284B20C, 0x8262E2BC);
constexpr uint32_t kPutBack2 = BO2_GAME_ADDR(0x8284B480, 0x8262E530);
constexpr uint32_t kPutBack3 = BO2_GAME_ADDR(0x8284B9C0, 0x8262EA70);

// --- Notifications ----------------------------------------------------------------------------------------------
constexpr uint32_t Bit(tr::Reason m) { return 1u << m; }

struct Sleeper {
  std::condition_variable cv;
  uint32_t interests = 0;
  uint32_t key = 0;  // for kWarningEnded and kWarningJob: only jobs with this key (0 = any)
  bool warned = false;
};

std::mutex g_mutex;                                                // guards g_sleeping and the sleeps
std::vector<Sleeper*> g_sleeping;
std::array<std::atomic<uint32_t>, tr::kNumWarnings> g_changes{};     // changes, by reason
std::array<std::atomic<int32_t>, tr::kNumWarnings> g_interested{};  // sleepers, by reason

/*
 * Sleepers with a lock of their own (bo2zm_waits_own_locks).
 *
 * With one shared lock, a profile on the Switch had each JQ worker spending 16 + 7.7 % of its samples blocked in
 * pthread_mutex_lock inside Sleep and 2 % inside Warn, all on g_mutex, which every sleep took on the way in and out
 * and every notifier held while it walked the sleepers and signalled each one; with the three cores full, a holder
 * that got preempted held up every thread that wanted to sleep or wake another. Here a thread sleeps in a slot of its
 * own (its mutex and condition); a notifier looks at the slots without a lock, takes only the mutex of a slot it
 * wakes, and signals after letting it go.
 *
 * No lost wake-up, as with the shared lock: the sleeper publishes its interests and then the slot (`active`, seq_cst)
 * before it checks the counters (in the wait's predicate, under its mutex), and a notifier bumps the counter before it
 * looks at `g_interested` and the slots (seq_cst both), so one of the two sees the other; `warned` is set under the
 * slot's mutex, so it cannot fall between the sleeper's check and its wait. A notifier that read a slot just before
 * its sleep ended can wake its next sleep early: harmless, the game's own check runs after every sleep. Sleeps that
 * started on the shared lock (the other A/B arm, or a thread without a slot) are woken through it as before.
 *
 * On the Switch the slot has no lock at all. With a mutex per slot the workers' waits on locks halved, but Main still
 * blocked in Warn on the mutex of a slot whose thread had just been woken and held it, preempted on a full core. So
 * there the sleeper sleeps on a word of its own with svcWaitForAddress (WaitIfEqual):
 * kIdle, kSleeping, kWarned. It stores kSleeping (seq_cst) before it checks the counters; a notifier that sees
 * kSleeping and wants the change moves it to kWarned with one compare-and-swap and signals the address. If that
 * happens before the sleeper reaches the kernel, the kernel finds the word changed and returns at once. Elsewhere
 * (the PC) the slot keeps its mutex and condition.
 */
constexpr int kSlots = 64;
constexpr uint32_t kIdle = 0;
constexpr uint32_t kSleeping = 1;
constexpr uint32_t kWarned = 2;
struct Slot {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> active{false};
  std::atomic<uint32_t> interests{0};
  std::atomic<uint32_t> key{0};
  bool warned = false;  // under mutex
  std::atomic<uint32_t> word{kIdle};  // the Switch: kIdle, kSleeping or kWarned
};
static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);

#if REX_PLATFORM_SWITCH
// Sleeps while *word is expected, at most timeout_ns; any return (woken, changed, timeout, spurious) re-reads.
void WaitWord(std::atomic<uint32_t>& word, uint32_t expected, int64_t timeout_ns) {
  constexpr uint32_t kArbitrationTypeWaitIfEqual = 2;
  svcWaitForAddress(&word, kArbitrationTypeWaitIfEqual, int64_t(int32_t(expected)), timeout_ns);
}

void WakeWord(std::atomic<uint32_t>& word) {
  constexpr uint32_t kSignalTypeSignal = 0;
  svcSignalToAddress(&word, kSignalTypeSignal, 0, 1);
}
#endif
std::array<Slot, kSlots> g_slots;
std::atomic<int> g_slots_taken{0};
std::atomic<int32_t> g_shared_sleepers{0};  // sleeps in progress on g_mutex
// Every 10 s: sleeps on each path and how they ended, and the wakes a notifier gave (reported from Sleep).
std::atomic<uint64_t> g_own_sleeps{0}, g_own_woken{0}, g_shared_sleeps{0}, g_shared_woken{0}, g_slot_wakes{0};
std::atomic<uint64_t> g_waits_report_ns{0};

// Whether a sleeper with these interests and key cares for a change of reason m with that key.
bool Wants(uint32_t interests, uint32_t sleeper_key, tr::Reason m, uint32_t key) {
  // A sleeper with a key (waiting for one job type or group) only cares for jobs of that key.
  const bool with_key = m == tr::kWarningEnded || m == tr::kWarningJob;
  return (interests & (1u << m)) && (!with_key || !sleeper_key || !key || sleeper_key == key);
}

bool OwnLocks() {
  static const bool on = REXCVAR_GET(bo2zm_waits_own_locks);
  static const bool ab = REXCVAR_GET(bo2zm_waits_own_locks_ab);
  if (ab) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & 1) != 0;
    }
  }
  return on;
}

// This thread's slot: taken at its first sleep, kept for the thread's life. -1: none left (the shared lock).
int MySlot() {
  thread_local int slot = -2;
  if (slot == -2) {
    const int i = g_slots_taken.fetch_add(1, std::memory_order_acq_rel);
    slot = i < kSlots ? i : -1;
  }
  return slot;
}

struct Seen {
  uint32_t v[tr::kNumWarnings];
};

Seen Look() {
  Seen x;
  for (int m = 0; m < tr::kNumWarnings; ++m) {
    x.v[m] = g_changes[m].load(std::memory_order_seq_cst);
  }
  return x;
}

bool Change(const Seen& seen, uint32_t interests) {
  for (int m = 0; m < tr::kNumWarnings; ++m) {
    if ((interests & (1u << m)) && g_changes[m].load(std::memory_order_seq_cst) != seen.v[m]) {
      return true;
    }
  }
  return false;
}

// After the change is visible in guest memory. Cheap when nobody sleeps on that reason.
void Warn(tr::Reason m, uint32_t key = 0) {
  g_changes[m].fetch_add(1, std::memory_order_seq_cst);
  if (tr::ReportActive()) {
    tr::g_warnings[m].fetch_add(1, std::memory_order_relaxed);
  }
  if (g_interested[m].load(std::memory_order_seq_cst) == 0) {
    return;
  }
  const int slots = std::min(g_slots_taken.load(std::memory_order_acquire), kSlots);
  for (int i = 0; i < slots; ++i) {
    Slot& s = g_slots[i];
#if REX_PLATFORM_SWITCH
    if (s.word.load(std::memory_order_seq_cst) != kSleeping ||
        !Wants(s.interests.load(std::memory_order_relaxed), s.key.load(std::memory_order_relaxed), m, key)) {
      continue;
    }
    uint32_t expected = kSleeping;
    if (s.word.compare_exchange_strong(expected, kWarned, std::memory_order_seq_cst)) {
      WakeWord(s.word);
      g_slot_wakes.fetch_add(1, std::memory_order_relaxed);
    }
    continue;
#endif
    if (!s.active.load(std::memory_order_seq_cst) ||
        !Wants(s.interests.load(std::memory_order_relaxed), s.key.load(std::memory_order_relaxed), m, key)) {
      continue;
    }
    bool wake = false;
    {
      std::lock_guard lock(s.mutex);
      if (s.active.load(std::memory_order_relaxed) && !s.warned) {
        s.warned = true;
        wake = true;
      }
    }
    if (wake) {
      s.cv.notify_one();
      g_slot_wakes.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (g_shared_sleepers.load(std::memory_order_seq_cst) == 0) {
    return;
  }
  std::lock_guard lock(g_mutex);
  for (Sleeper* d : g_sleeping) {
    if (Wants(d->interests, d->key, m, key)) {
      d->warned = true;
      d->cv.notify_one();
    }
  }
}

bool AnyActive() {
  static const bool active = REXCVAR_GET(bo2zm_blocking_waits);
  return active;
}

std::atomic<bool>& FenceWakeSwitch() {
  static std::atomic<bool> on{REXCVAR_GET(bo2zm_waits_fence_wake)};
  return on;
}
bool FenceWake() {
  return FenceWakeSwitch().load(std::memory_order_relaxed);
}

std::chrono::microseconds Limit() {
  static const auto limit = std::chrono::microseconds(REXCVAR_GET(bo2zm_waits_max_us));
  return limit;
}

// The game's own pause between two looks at an unmet dependency: 50,000 guest timebase ticks (~1 ms), plus a
// little so that the look is due when the thread wakes.
std::chrono::microseconds PauseDependency() {
  static const auto pause = std::chrono::microseconds(
      50000ull * 1000000ull / std::max<uint64_t>(1, rex::chrono::Clock::guest_tick_frequency()) + 100);
  return pause;
}

uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Every 10 s, from whichever thread finishes a sleep first after that: sleeps on each path.
void ReportWaits() {
  const uint64_t now = NowNs();
  uint64_t last = g_waits_report_ns.load(std::memory_order_relaxed);
  if (last == 0) {
    g_waits_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10'000'000'000ull ||
      !g_waits_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t own = g_own_sleeps.exchange(0, std::memory_order_relaxed);
  const uint64_t own_woken = g_own_woken.exchange(0, std::memory_order_relaxed);
  const uint64_t shared = g_shared_sleeps.exchange(0, std::memory_order_relaxed);
  const uint64_t shared_woken = g_shared_woken.exchange(0, std::memory_order_relaxed);
  const uint64_t wakes = g_slot_wakes.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[waits] last {:.0f} s: {} sleeps with a lock of their own ({} woken by a change, {} slots taken), {} on "
              "the shared lock ({} woken); {} wakes given to slots",
              double(now - last) / 1e9, own, own_woken, std::min(g_slots_taken.load(), kSlots), shared,
              shared_woken, wakes);
}

// Sleeps until one of 'interests' changed after 'seen' was read, or 'limit'. The interest is published under the
// lock before the counters are checked again; a notifier bumps the counter before it looks for sleepers, so one of
// the two always sees the other.
// Returns false when the sleep ended by its timeout.
bool Sleep(tr::Room room, const Seen& seen, uint32_t interests, uint32_t key,
            std::chrono::microseconds limit) {
  static const uint32_t rooms = uint32_t(REXCVAR_GET(bo2zm_wait_sites));
  if (!(rooms & (1u << room))) {
    return true;
  }
  thread_local Sleeper yo;
  const bool report = tr::ReportActive();
  const uint64_t t0 = report ? NowNs() : 0;
  bool warned;
  const int slot = OwnLocks() ? MySlot() : -1;
  if (slot >= 0) {
    // A lock of its own (see "Sleepers with a lock of their own").
    Slot& s = g_slots[slot];
    s.interests.store(interests, std::memory_order_relaxed);
    s.key.store(key, std::memory_order_relaxed);
    for (int m = 0; m < tr::kNumWarnings; ++m) {
      if (interests & (1u << m)) {
        g_interested[m].fetch_add(1, std::memory_order_seq_cst);
      }
    }
#if REX_PLATFORM_SWITCH
    {
      // A word of its own, no lock (see "Sleepers with a lock of their own").
      s.word.store(kSleeping, std::memory_order_seq_cst);
      const uint64_t deadline = NowNs() + uint64_t(limit.count()) * 1000ull;
      warned = false;
      for (;;) {
        if (s.word.load(std::memory_order_seq_cst) == kWarned || Change(seen, interests)) {
          warned = true;
          break;
        }
        const uint64_t now = NowNs();
        if (now >= deadline) {
          break;
        }
        WaitWord(s.word, kSleeping, int64_t(deadline - now));
      }
      s.word.store(kIdle, std::memory_order_seq_cst);
    }
#else
    {
      std::unique_lock lock(s.mutex);
      s.warned = false;
      s.active.store(true, std::memory_order_seq_cst);
      warned = s.cv.wait_for(lock, limit, [&] { return s.warned || Change(seen, interests); });
      s.active.store(false, std::memory_order_relaxed);
    }
#endif
    for (int m = 0; m < tr::kNumWarnings; ++m) {
      if (interests & (1u << m)) {
        g_interested[m].fetch_sub(1, std::memory_order_seq_cst);
      }
    }
    g_own_sleeps.fetch_add(1, std::memory_order_relaxed);
    if (warned) {
      g_own_woken.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    std::unique_lock lock(g_mutex);
    g_shared_sleepers.fetch_add(1, std::memory_order_seq_cst);
    for (int m = 0; m < tr::kNumWarnings; ++m) {
      if (interests & (1u << m)) {
        g_interested[m].fetch_add(1, std::memory_order_seq_cst);
      }
    }
    yo.interests = interests;
    yo.key = key;
    yo.warned = false;
    g_sleeping.push_back(&yo);
    warned = yo.cv.wait_for(lock, limit, [&] { return yo.warned || Change(seen, interests); });
    g_sleeping.erase(std::find(g_sleeping.begin(), g_sleeping.end(), &yo));
    for (int m = 0; m < tr::kNumWarnings; ++m) {
      if (interests & (1u << m)) {
        g_interested[m].fetch_sub(1, std::memory_order_seq_cst);
      }
    }
    g_shared_sleepers.fetch_sub(1, std::memory_order_seq_cst);
    g_shared_sleeps.fetch_add(1, std::memory_order_relaxed);
    if (warned) {
      g_shared_woken.fetch_add(1, std::memory_order_relaxed);
    }
  }
  ReportWaits();
  if (report) {
    auto& h = tr::JobThread();
    const uint64_t ns = NowNs() - t0;
    h.stalls.fetch_add(1, std::memory_order_relaxed);
    h.ns_stalls.fetch_add(ns, std::memory_order_relaxed);
    auto& s = tr::g_rooms[room];
    s.times.fetch_add(1, std::memory_order_relaxed);
    s.ns.fetch_add(ns, std::memory_order_relaxed);
    if (warned) {
      h.stalls_warned.fetch_add(1, std::memory_order_relaxed);
    } else {
      s.exhausted.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return warned;
}

// --- What a thread may take -------------------------------------------------------------------------------------
// Jobs in the queues a slot takes from: its own, the registered ones and the preferred one (sub_8284AED8).
bool HasTakeable(const uint8_t* base, uint32_t slot) {
  bool preferred = false;
  if (tr::Read32(base, slot + tr::kSlotOwn + tr::kQueueCount)) {
    return true;
  }
  for (int k = 0; k < tr::kQueuesPerSlot; ++k) {
    const uint32_t c = tr::Read32(base, slot + tr::kSlotQueues + 4 * k);
    if (c && tr::Read32(base, c + tr::kQueueCount)) {
      return true;
    }
    preferred |= c == tr::kQueuePreferred;
  }
  return !preferred && tr::Read32(base, tr::kQueuePreferred + tr::kQueueCount);
}

// A helper slot (sub_8284BB48 with no queue of its own) takes from the default and the preferred queue.
bool HasTakeableHelp(const uint8_t* base) {
  return tr::Read32(base, tr::kDefaultQueue + tr::kQueueCount) ||
         tr::Read32(base, tr::kQueuePreferred + tr::kQueueCount);
}

// GetCurrentThreadId as the game reads it (sub_827D4AB0: [[r13 + 256] + 332]).
uint32_t IdThreadGuest(const PPCContext& ctx, const uint8_t* base) {
  return tr::Read32(base, tr::Read32(base, ctx.r13.u32 + 256) + 332);
}

bool HaveDevice(const PPCContext& ctx, const uint8_t* base) {
  return tr::Read32(base, kDeviceOwner) == IdThreadGuest(ctx, base);
}

// --- Per thread ---------------------------------------------------------------------------------------------------
struct ThreadStatus {
  bool taken = false;        // its last pick took a job; by the next hook point the job is over
  uint32_t key = 0;         // that job's key (group, or type descriptor + 16)
  uint32_t type_key = 0;    // its type descriptor + 16 (told too when the key is a group)
  uint32_t group = 0;         // the group sub_82493690 waits for (0 outside it)
  bool last_empty = false;  // its last pick found nothing
  Seen seen_choice{};     // changes seen before its last pick
  int loops = 0;             // inside a help loop (sub_824935E8 / sub_82493548)
  // A poll inside a help loop that found the wait not over while this thread held the device lock: the sleep
  // waits until the loop releases the lock (sub_828DB270).
  bool postponed = false;
  Seen seen_postponed{};
  uint32_t interests_postponed = 0;
  uint32_t slot_worker = 0;  // a JQ worker's slot (seen in its wait, sub_8284A088); 0 on every other thread
  int misses_section = 0;     // tries of critical section 6 that failed in a row (sub_828EFC68)
  int putbacks_row = 0;       // jobs this thread put back in a row (sub_8284C3B8)
};
thread_local ThreadStatus t;

// A job this thread took is over (or was put back in its queue): tell whoever waits for its type or a job.
void WarnYesEnded() {
  if (t.taken) {
    t.taken = false;
    t.putbacks_row = 0;
    Warn(tr::kWarningEnded, t.key);
    if (t.type_key != t.key) {
      Warn(tr::kWarningEnded, t.type_key);
    }
  }
}

// Times a hooked game wait for the report: who (the function) and from where (its caller).
template <typename F>
void Measure(uint32_t who, PPCContext& ctx, uint8_t* base, F original) {
  if (!tr::ReportActive()) [[likely]] {
    original(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const uint64_t t0 = NowNs();
  original(ctx, base);
  tr::AnnotateWait(who, lr, NowNs() - t0);
}

// A poll inside a help loop: when the wait is not over and the thread has no job it may take, sleep until the
// events this poll checks or a job may have changed, then poll again (the loop goes on from that answer).
template <typename F>
void PollBlocking(tr::Room room, PPCContext& ctx, uint8_t* base, F original, uint32_t interests) {
  const uint32_t r3 = ctx.r3.u32;
  const Seen seen = Look();
  original(ctx, base);
  if ((ctx.r3.u32 & 0xFF) || !AnyActive() || t.loops == 0 || HasTakeableHelp(base)) {
    return;
  }
  if (HaveDevice(ctx, base)) {
    t.postponed = true;  // sleep after the loop releases the device lock
    t.seen_postponed = seen;
    t.interests_postponed = interests;
    return;
  }
  Sleep(room, seen, interests, 0, Limit());
  ctx.r3.u64 = r3;
  original(ctx, base);
}

constexpr uint32_t kInterestsHelp = Bit(tr::kWarningJob) | Bit(tr::kWarningDeferred);

}  // namespace

// --- Ring progress from the native renderer (declared in bo2zm_native_render.h) ----------------------------------
namespace bo2zm::native {
void WarnProgressRing() {
  Warn(tr::kWarningRing);
}
}  // namespace bo2zm::native

namespace bo2zm::waits {
bool FenceWake() { return ::FenceWake(); }
void SetFenceWake(bool on) { FenceWakeSwitch().store(on, std::memory_order_relaxed); }
}  // namespace bo2zm::waits

// --- Fence writes from the SDK command processor (the Metal and Vulkan renderers) ---------------------------------
// The SDK writes a D3D fence (EVENT_WRITE_SHD) as it reads the ring; the native renderer warns on its own above. Told
// as ring progress: the put-back and ring-poll sleepers already wait for that reason.
namespace {
void WarnFenceWritten() {
  if (FenceWake()) {  // off: the put-back sleeps are as before (the A arm of an A/B)
    Warn(tr::kWarningRing);
  }
}
[[maybe_unused]] const bool g_fence_hook_set = [] {
  rex::graphics::SetGuestWriteHook(&WarnFenceWritten);
  return true;
}();
}  // namespace

// --- The picker: sub_8284AED8(slot, filter, record out, flag) returns 1 with a job, or yields and returns 0 --------
REX_HOOK_RAW(sub_8284AED8) {
  bo2zm::benchmark::ApplyOwnPriority();
  const uint32_t slot = ctx.r3.u32, filter = ctx.r4.u32, reg_index = ctx.r5.u32, lr = uint32_t(ctx.lr);
  WarnYesEnded();
  t.seen_choice = Look();
  __imp__sub_8284AED8(ctx, base);
  const bool with_job = ctx.r3.u32 & 0xFF;
  t.last_empty = !with_job;
  bool no_dependency = false;  // a job whose counter has not reached its target: it goes back, nothing ends
  if (with_job) {
    const uint32_t dependency = tr::Read32(base, reg_index + tr::kJobDependency);
    no_dependency =
        dependency && tr::Read32(base, dependency) < tr::Read32(base, reg_index + tr::kJobGoal);
    t.taken = !no_dependency;
    const uint32_t group = tr::Read32(base, reg_index + tr::kJobGroup);
    t.type_key = tr::Read32(base, reg_index + tr::kJobType) + 16;
    t.key = group ? group : t.type_key;
  }
  if (tr::ReportActive()) {
    tr::Boot(base);
    auto& h = tr::JobThread();
    h.choose.fetch_add(1, std::memory_order_relaxed);
    if (!with_job) {
      h.choose_empty[tr::ClassifyEmpty(base, slot)].fetch_add(1, std::memory_order_relaxed);
    } else if (no_dependency) {
      h.choose_dependent.fetch_add(1, std::memory_order_relaxed);
      tr::AnnotateDependency(tr::Read32(base, reg_index + tr::kJobDependency),
                            tr::Read32(base, reg_index + tr::kJobType));
    }
  }
  // A job whose dependency is not met: the game would put it back and pick again at once, spinning through the
  // queue, and only read the counter again 50,000 timebase ticks after its last look (sub_8284AFB8 0x8284B0FC).
  // Hold it and sleep about that long, or less if a job is pushed (the game then puts this one back and takes the
  // new one). Afterwards the game's own check runs on it as before.
  if (no_dependency && AnyActive()) {
    const Seen before = Look();
    const uint32_t dependency = tr::Read32(base, reg_index + tr::kJobDependency);
    if (tr::Read32(base, dependency) < tr::Read32(base, reg_index + tr::kJobGoal)) {
      Sleep(tr::kRoomDependency, before, kInterestsHelp | Bit(tr::kWarningEvent), 0, PauseDependency());
    }
  }
  // "Wait for a job type" (sub_8284B730) found nothing: when none of its jobs is queued, or none is where slot 0
  // takes from, sleep until one of them ends or a job is pushed. sub_8284B730 then checks its counters again.
  if (!with_job && AnyActive() && (lr == tr::kChooseWait1 || lr == tr::kChooseWait2)) {
    const uint32_t counters = filter ? filter : tr::kInQueue;
    if (tr::Read32(base, counters) == 0 || !HasTakeable(base, slot)) {
      Sleep(tr::kRoomType, t.seen_choice, kInterestsHelp | Bit(tr::kWarningEnded), filter, Limit());
    }
  }
}

// NtYieldExecution, through the game's wrapper (counted for the report).
REX_HOOK_RAW(sub_827D8DD8) {
  if (tr::ReportActive()) {
    tr::JobThread().yielded.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_827D8DD8(ctx, base);
}

// The workers' wait: sub_8284A088(slot) returns 0 to quit. It follows an empty pick. While this worker has nothing
// it may take (nothing queued, or only in queues waiting for release), and the job system is not quitting or
// asking the workers to stay awake (+0x8479D0B4), sleep until a job is pushed or a queue is put on the release list
// (then return, so that the next pick releases it). The game's wait then runs and returns at once.
REX_HOOK_RAW(sub_8284A088) {
  WarnYesEnded();
  const uint32_t slot = ctx.r3.u32;
  t.slot_worker = slot;  // only the JQ workers' loop (sub_8284AFB8) waits here
  const bool report = tr::ReportActive();
  const uint64_t t0 = report ? NowNs() : 0;
  if (report) {
    auto& h = tr::JobThread();
    h.wait_worker.fetch_add(1, std::memory_order_relaxed);
    if (tr::Read32(base, tr::kInQueue) != 0) {
      h.wait_no_sleep.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (AnyActive()) {
    Seen seen = t.seen_choice;  // read before the empty pick: nothing pushed since is lost
    for (;;) {
      if (base[tr::kExit] || tr::Read32(base, tr::kPause) || HasTakeable(base, slot)) {
        break;
      }
      // A queue went on the release list and jobs are queued somewhere: the next pick may release them.
      if (g_changes[tr::kWarningDeferred].load(std::memory_order_seq_cst) != t.seen_choice.v[tr::kWarningDeferred] &&
          tr::Read32(base, tr::kInQueue) != 0) {
        break;
      }
      // A timeout with jobs queued somewhere: pick once anyway. A queue on the release list that a pick could not
      // release yet (a push into it was still under way, its +64 count) is only released by a pick.
      if (!Sleep(tr::kRoomWorker, seen, kInterestsHelp, 0, 4 * Limit()) && tr::Read32(base, tr::kInQueue)) {
        break;
      }
      seen = Look();
    }
  }
  __imp__sub_8284A088(ctx, base);
  if (report) {
    tr::JobThread().ns_wait_worker.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  }
}

// Push: sub_8284A910(record, queue) counts it, pushes it and pulses the job event. Then: a job to take.
REX_HOOK_RAW(sub_8284A910) {
  const uint32_t job = ctx.r3.u32, queue = ctx.r4.u32;
  if (tr::ReportActive()) {
    tr::JobThread().queued.fetch_add(1, std::memory_order_relaxed);
    tr::AnnotateQueued(base, job, queue);
  }
  const uint32_t group = tr::Read32(base, job + tr::kJobGroup);
  const uint32_t key = group ? group : tr::Read32(base, job + tr::kJobType) + 16;
  __imp__sub_8284A910(ctx, base);
  Warn(tr::kWarningJob, key);
}

// The lock-free list push (sub_8284C3B8(list, record)). From the loops right after a run, it puts the job back
// (counted for the report); after kPutBacksRow of those in a row, see case 6 in the header comment.
REX_HOOK_RAW(sub_8284C3B8) {
  const uint32_t lr = uint32_t(ctx.lr);
  const bool put_back = lr == kPutBack1 || lr == kPutBack2 || lr == kPutBack3;
  if (put_back && tr::ReportActive()) {
    tr::JobThread().returned.fetch_add(1, std::memory_order_relaxed);
    tr::AnnotateReturned(tr::Read32(base, ctx.r4.u32 + tr::kJobType));
  }
  __imp__sub_8284C3B8(ctx, base);
  if (!put_back) {
    return;
  }
  t.taken = false;  // it did not end
  const bool worker = t.slot_worker != 0;
  if (!AnyActive() || (!worker && t.loops == 0) || ++t.putbacks_row < kPutBacksRow) {
    return;
  }
  t.putbacks_row = 0;
  if (!worker && HaveDevice(ctx, base)) {
    return;
  }
  uint32_t interests = kInterestsHelp | Bit(tr::kWarningEnded) | Bit(tr::kWarningLock);
  if (!worker) {
    interests |= Bit(tr::kWarningEvent) | Bit(tr::kWarningRing);
  } else if (FenceWake()) {
    // The jobs a worker puts back wait for a GPU fence (sub_828AA590): wake when the command processor writes one.
    interests |= Bit(tr::kWarningRing);
  }
  Sleep(tr::kRoomPutBack, t.seen_choice, interests, 0, Limit());
}

// Case 7: the wait for the culling jobs (sub_828DED00(job, data) returns 1, "again", while 0x82A99A30 is not zero).
bool CullWait() {
  static const int factor = REXCVAR_GET(bo2zm_waits_cull_ab);
  if (factor != 0) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  static const bool on = REXCVAR_GET(bo2zm_waits_cull);
  return on;
}

REX_EXTERN(__imp__sub_828DED00);
REX_HOOK_RAW(sub_828DED00) {
  constexpr uint32_t kCullPending = BO2_GAME_ADDR(0x82A99A30, 0x8285D640);  // type 0x82A99A20 + 16: also the key their end is told with
  const auto pending = [&] { return (tr::Read32(base, kCullPending) | tr::Read32(base, kCullPending + 4)) != 0; };
  if (CullWait() && AnyActive() && pending() && !HaveDevice(ctx, base)) {
    const Seen seen = Look();
    if (pending()) {
      Sleep(tr::kRoomCull, seen, Bit(tr::kWarningEnded), kCullPending, Limit());
    }
  }
  __imp__sub_828DED00(ctx, base);
}

// A queue put on the release list (sub_8284AB18, run from a GPU callback through sub_828B56A0): the next pick
// (sub_8284AC48) moves its jobs.
REX_HOOK_RAW(sub_8284AB18) {
  __imp__sub_8284AB18(ctx, base);
  Warn(tr::kWarningDeferred);
}

// A released queue's jobs moved to their destination queue (sub_8284AB88): jobs to take.
REX_HOOK_RAW(sub_8284AB88) {
  __imp__sub_8284AB88(ctx, base);
  Warn(tr::kWarningJob);
}

// Help: sub_8284BB48(callback, argument, filter, queue) runs jobs from a helper slot while the callback allows.
// Called from sub_82493690's loops ("help until a group is done") and its pick found nothing: sleep until a job or
// a job's end; the loop then checks the group again.
REX_HOOK_RAW(sub_8284BB48) {
  const uint32_t lr = uint32_t(ctx.lr);
  const bool report = tr::ReportActive();
  const uint64_t t0 = report ? NowNs() : 0;
  __imp__sub_8284BB48(ctx, base);
  WarnYesEnded();
  if (report) {
    auto& h = tr::JobThread();
    h.helps.fetch_add(1, std::memory_order_relaxed);
    h.ns_helps.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  }
  if (AnyActive() && t.last_empty && (lr == kHelpInGroup1 || lr == kHelpInGroup2) && !HasTakeableHelp(base)) {
    Sleep(tr::kRoomGroup, t.seen_choice, kInterestsHelp | Bit(tr::kWarningEnded), t.group, Limit());
  }
}

// Wait for a job type or group: sub_8284B730(counters, limit). The sleep is in the picker's hook.
REX_HOOK_RAW(sub_8284B730) {
  const bool report = tr::ReportActive();
  const uint32_t lr = uint32_t(ctx.lr);
  const uint64_t t0 = report ? NowNs() : 0;
  __imp__sub_8284B730(ctx, base);
  WarnYesEnded();
  if (report) {
    auto& h = tr::JobThread();
    const uint64_t ns = NowNs() - t0;
    h.waits_type.fetch_add(1, std::memory_order_relaxed);
    h.ns_waits_type.fetch_add(ns, std::memory_order_relaxed);
    tr::AnnotateWait(0x8284B730, lr, ns);
  }
}

// The help loops: mark the thread as inside one, so that their polls may sleep.
REX_HOOK_RAW(sub_824935E8) {
  ++t.loops;
  Measure(0x824935E8, ctx, base, __imp__sub_824935E8);
  --t.loops;
  t.postponed = false;
}
REX_HOOK_RAW(sub_82493548) {
  ++t.loops;
  Measure(0x82493548, ctx, base, __imp__sub_82493548);
  --t.loops;
  t.postponed = false;
}

// The other game-level wrappers, timed by caller for the report.
REX_HOOK_RAW(sub_82493190) { Measure(0x82493190, ctx, base, __imp__sub_82493190); }  // wait for a job type
// sub_827D4BD8(handle, ...): the game's WaitForSingleObject wrapper, timed by caller for the report. Main blocks in it
// on the Backend's events: from sub_82428A78 on the handle at 0x83AF7380, and from sub_8240FDC0.
REX_HOOK_RAW(sub_827D4BD8) { Measure(0x827D4BD8, ctx, base, __imp__sub_827D4BD8); }
// Help until a group (r3) is done: its sleeps (in sub_8284BB48's hook) wait for that group's jobs.
REX_HOOK_RAW(sub_82493690) {
  const uint32_t earlier = t.group;
  t.group = ctx.r3.u32;
  Measure(0x82493690, ctx, base, __imp__sub_82493690);
  t.group = earlier;
}
REX_HOOK_RAW(sub_82493240) { Measure(0x82493240, ctx, base, __imp__sub_82493240); }  // help once with a job type
// Help once. From the Server's wait for its client jobs, with nothing to take: sleep until one of those jobs ends
// (or one is pushed); the Server then checks its flags again.
REX_HOOK_RAW(sub_824931E8) {
  const uint32_t lr = uint32_t(ctx.lr);
  Measure(0x824931E8, ctx, base, __imp__sub_824931E8);
  if (AnyActive() && lr == kHelpServer && t.last_empty && !HasTakeableHelp(base)) {
    Sleep(tr::kRoomServer, t.seen_choice, kInterestsHelp | Bit(tr::kWarningEnded), kKeyServer,
           Limit());
  }
}

// The polls of the help loops.
// sub_82428ED8(ms): WaitForSingleObject(frame event 0x83AF71DC, ms) == 0. The Backend's "next frame" wait.
REX_HOOK_RAW(sub_82428ED8) {
  bo2zm::benchmark::ApplyOwnPriority();
  const bool poll = ctx.r3.u32 == 0;
#if REX_PLATFORM_SWITCH
  // The older, simpler hook, kept for when the blocking waits are off: after 32 idle polls in a row (no job queued
  // anywhere), sleep 200 us before each poll.
  if (poll && !AnyActive()) {
    thread_local uint32_t idle = 0;
    if (tr::Read32(base, tr::kInQueue) != 0) {
      idle = 0;
    } else if (++idle > 32) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
#endif
  if (poll) {
    PollBlocking(tr::kRoomPollFrame, ctx, base, __imp__sub_82428ED8,
                     kInterestsHelp | Bit(tr::kWarningEvent));
  } else {
    __imp__sub_82428ED8(ctx, base);
  }
  if (tr::ReportActive() && poll) {
    auto& h = tr::JobThread();
    h.polls.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r3.u32 & 0xFF) {
      h.polls_ready.fetch_add(1, std::memory_order_relaxed);
    }
  }
}
// sub_82428840(): WaitForSingleObject("frame done" event 0x83AF7378, 0) == 0. Main waits for the Backend
// (sub_828BADC8 through sub_82493548).
REX_HOOK_RAW(sub_82428840) {
  PollBlocking(tr::kRoomPollDone, ctx, base, __imp__sub_82428840, kInterestsHelp | Bit(tr::kWarningEvent));
}
// sub_828DA6F0(): 1 when the ring's pending count is 0 (sub_82171DD8). The Backend waits for the GPU
// (sub_828DAAA8 through sub_824935E8). Only the native renderer notifies ring progress.
REX_HOOK_RAW(sub_828DA6F0) {
  if (bo2zm::native::IsActive()) {
    PollBlocking(tr::kRoomPollRing, ctx, base, __imp__sub_828DA6F0,
                     kInterestsHelp | Bit(tr::kWarningRing));
  } else {
    __imp__sub_828DA6F0(ctx, base);
  }
}

// sub_828DB270 releases the D3D device lock. In a help loop, after a poll that had to wait while the lock was held:
// sleep now, with the lock released. The loop then helps (if jobs are queued), takes the lock and polls again.
REX_HOOK_RAW(sub_828DB270) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_828DB270(ctx, base);
  if (t.postponed && (lr == kReleaseInLoop1 || lr == kReleaseInLoop2)) {
    t.postponed = false;
    if (!HasTakeableHelp(base)) {
      Sleep(tr::kRoomAfterRelease, t.seen_postponed, t.interests_postponed, 0, Limit());
    }
  }
}

// The Backend's events: frame ready (sub_82428878, and sub_82429748 which also hands over the frame's data) and
// frame done (sub_82429708).
// The game's "leave critical section by index" (r3; its table at 0x83DE0218, 28 bytes each): section 6 left wakes
// the workers waiting to try it again (sub_828EFC68 below).
constexpr uint32_t kSectionModels = 6;
REX_HOOK_RAW(sub_824A0BC0) {
  const uint32_t index = ctx.r3.u32;
  __imp__sub_824A0BC0(ctx, base);
  if (index == kSectionModels) {
    Warn(tr::kWarningLock);
  }
}

// Critical section 6, tried (r3 = 1 when taken). See case 5 in the header comment.
REX_HOOK_RAW(sub_828EFC68) {
  const Seen seen = Look();
  __imp__sub_828EFC68(ctx, base);
  if (ctx.r3.u32 & 0xFF) {
    t.misses_section = 0;
    return;
  }
  if (!AnyActive() || t.slot_worker == 0 || ++t.misses_section < kMissesSection) {
    return;
  }
  t.misses_section = 0;
  Sleep(tr::kRoomLock, seen, Bit(tr::kWarningLock) | kInterestsHelp, 0, Limit());
  __imp__sub_828EFC68(ctx, base);
}

REX_HOOK_RAW(sub_82428878) {
  __imp__sub_82428878(ctx, base);
  Warn(tr::kWarningEvent);
}
REX_HOOK_RAW(sub_82429748) {
  __imp__sub_82429748(ctx, base);
  Warn(tr::kWarningEvent);
}
REX_HOOK_RAW(sub_82429708) {
  __imp__sub_82429708(ctx, base);
  Warn(tr::kWarningEvent);
}

// --- The sound mixer's jobs ---------------------------------------------------------------------------------------
// BO2's sound mixer thread ("Sound Mix") hands each mix step to the job system and waits for it: five functions push
// one job on the preferred queue and then wait for its event with no timeout (sub_82428468). Their job types, by the
// names in their descriptors: sd_mix_voice (sub_82442B90), sd_mix_master (sub_8243F3C8), sd_mix_radverb
// (sub_8243F538), sd_voice_active (sub_8258A368), sd_voice_free (sub_8258A4C0). In a match the workers are busy with
// the frame at the guest priority, so every mix step waited behind them and the sound dropped out: on the Switch only
// ~700 blocks were mixed per 10 s instead of ~1,900, with the mixer itself already at the audio priority. So the
// waiting thread first runs the jobs of that type itself, with the game's own "wait for a job
// type" (sub_82493190, which Main uses from ~20 sites), at its own priority; then it waits for the event as before,
// by then set. A job a worker already took is waited for in sub_8284B730's sleep (above).
REXCVAR_DEFINE_BOOL(bo2zm_sound_without_queue, true, "BO2",
                    "The sound mixer runs its own mix jobs (sd_mix_voice, sd_mix_master, ...) while it waits for "
                    "them, instead of waiting for a job worker to take them. false = the game's own wait");

REX_EXTERN(__imp__sub_82442B90);
REX_EXTERN(__imp__sub_8243F3C8);
REX_EXTERN(__imp__sub_8243F538);
REX_EXTERN(__imp__sub_8258A368);
REX_EXTERN(__imp__sub_8258A4C0);
REX_EXTERN(__imp__sub_82428468);
REX_EXTERN(sub_82493190);  // hooked above (Measure)

namespace {

// Inside one of the five mix steps: the job definition it pushes (0 elsewhere). The definitions come from the code
// of each step (the lis/addi before its push).
thread_local uint32_t t_step_sound = 0;

// Every 10 s, from the thread that ran them: how many mix steps ran on the waiting thread, and their time.
std::atomic<uint32_t> g_steps_sound{0};
std::atomic<uint64_t> g_ns_sound{0};
std::atomic<uint64_t> g_max_ns_sound{0};
std::atomic<uint64_t> g_next_summary_sound{0};

template <typename F>
void SoundStep(PPCContext& ctx, uint8_t* base, uint32_t definition, F original) {
  const uint32_t earlier = t_step_sound;
  t_step_sound = REXCVAR_GET(bo2zm_sound_without_queue) ? definition : 0;
  original(ctx, base);
  t_step_sound = earlier;
}

void SummarizeSound(uint64_t now) {
  uint64_t next = g_next_summary_sound.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_summary_sound.compare_exchange_strong(next, now + 10'000'000'000ull);
    return;
  }
  if (now < next ||
      !g_next_summary_sound.compare_exchange_strong(next, now + 10'000'000'000ull)) {
    return;
  }
  const uint32_t steps = g_steps_sound.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_ns_sound.exchange(0, std::memory_order_relaxed);
  const uint64_t max_ns = g_max_ns_sound.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[sound] 10 s: {} mix steps run by the mixer itself, {:.1f} ms in total, longest {:.2f} ms", steps,
              double(ns) / 1e6, double(max_ns) / 1e6);
}

}  // namespace

REX_HOOK_RAW(sub_82442B90) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982C70, 0x82746E70), __imp__sub_82442B90); }  // sd_mix_voice
REX_HOOK_RAW(sub_8243F3C8) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982B70, 0x82746D70), __imp__sub_8243F3C8); }  // sd_mix_master
REX_HOOK_RAW(sub_8243F538) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82982BF0, 0x82746DF0), __imp__sub_8243F538); }  // sd_mix_radverb
REX_HOOK_RAW(sub_8258A368) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82A7D1D0, 0x82848F50), __imp__sub_8258A368); }  // sd_voice_active
REX_HOOK_RAW(sub_8258A4C0) { SoundStep(ctx, base, BO2_GAME_ADDR(0x82A7D1F0, 0x82848F70), __imp__sub_8258A4C0); }  // sd_voice_free

// WaitForSingleObject(*r3, INFINITE). In a mix step, run the step's jobs first (r3 and lr kept for the wait).
REX_HOOK_RAW(sub_82428468) {
  if (const uint32_t definition = t_step_sound) [[unlikely]] {
    t_step_sound = 0;  // once; a mix step inside the jobs it runs sets its own
    const uint64_t event = ctx.r3.u64;
    const uint64_t lr = ctx.lr;
    const uint64_t t0 = NowNs();
    ctx.r3.u64 = definition;
    sub_82493190(ctx, base);
    ctx.r3.u64 = event;
    ctx.lr = lr;
    const uint64_t now = NowNs();
    const uint64_t ns = now - t0;
    g_steps_sound.fetch_add(1, std::memory_order_relaxed);
    g_ns_sound.fetch_add(ns, std::memory_order_relaxed);
    uint64_t max_ns = g_max_ns_sound.load(std::memory_order_relaxed);
    while (ns > max_ns && !g_max_ns_sound.compare_exchange_weak(max_ns, ns, std::memory_order_relaxed)) {
    }
    SummarizeSound(now);
  }
  __imp__sub_82428468(ctx, base);
}
