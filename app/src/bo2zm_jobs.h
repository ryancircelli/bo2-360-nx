// BO2's job system: the addresses and layouts the hooks in bo2zm_waits.cpp rely on, and an optional periodic
// report of its state (bo2zm_jobs_report_s, off by default). Each address below was found in the game's code.
//
// In short (TU18 default_mp.xex): a job is a record (+12 its type descriptor, +16 an optional group of counters)
// pushed onto a queue (sub_8284A910, which then pulses the job event). Each hardware thread of the 360 has a slot
// (worker slots for the masked hardware threads, sub_8284BC40); a slot takes jobs from its own queue and from the
// queues registered for its hardware thread (the default queue and the preferred queue, sub_82849D08). The picker
// sub_8284AED8 tries those queues and yields (NtYieldExecution) when none has a job it may take. Threads that are not
// workers (Main, Backend, Server) help through a helper slot (sub_8284BB48) or while waiting for a job type to finish
// (sub_8284B730, which uses slot 0).

#pragma once

#include "bo2zm_game.h"
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/platform.h>
#include <rex/system/xmemory.h>

namespace bo2zm::jobs {

// --- Globals ----------------------------------------------------------------------------------------------------
constexpr uint32_t kStats = BO2_GAME_ADDR(0x82A97D90, 0x8285B9A0);      // stats block; its first word points to itself
constexpr uint32_t kInQueue = kStats + 64;    // jobs queued in any queue (sub_82849EC8(0), sub_82849F90(0))
constexpr uint32_t kInRunning = kStats + 68;  // jobs taken and not finished (sub_82849EE8(0))
constexpr uint32_t kMaskThreads = BO2_GAME_ADDR(0x82A97D88, 0x8285B998);      // hardware threads that get a worker slot (sub_8284BC40)
constexpr uint32_t kNumSlots = BO2_GAME_ADDR(0x8479D0A0, 0x83FCC220);        // worker slots
constexpr uint32_t kSlots = BO2_GAME_ADDR(0x8479D0A4, 0x83FCC224);           // -> the worker slots (slot 0 is hardware thread 0, Main)
constexpr uint32_t kSlotsHelp = BO2_GAME_ADDR(0x8479D0A8, 0x83FCC228);      // -> 16 helper slots (sub_8284BB48)
constexpr uint32_t kHelpsInOngoing = BO2_GAME_ADDR(0x8479D0AC, 0x83FCC22C);     // helper slots in use
constexpr uint32_t kPause = BO2_GAME_ADDR(0x8479D0B4, 0x83FCC234);             // nonzero: workers do not wait on the event (sub_8284A088)
constexpr uint32_t kWorkersWaiting = BO2_GAME_ADDR(0x8479D0B8, 0x83FCC238);  // workers inside the event wait (sub_8284A088)
constexpr uint32_t kEventJobs = BO2_GAME_ADDR(0x8479D0CC, 0x83FCC24C);    // the job event's handle
constexpr uint32_t kExit = BO2_GAME_ADDR(0x8479D0D0, 0x83FCC250);             // byte: workers leave their loop
constexpr uint32_t kDefaultQueue = BO2_GAME_ADDR(0x8479D0E0, 0x83FCC260);    // registered for every hardware thread (mask 63)
constexpr uint32_t kListDeferred = BO2_GAME_ADDR(0x8479D1E0, 0x83FCC360);    // queues waiting to be released (sub_8284AB18, sub_8284AC48)
constexpr uint32_t kQueuePreferred = BO2_GAME_ADDR(0x8479D208, 0x83FCC388);    // registered for every hardware thread; tried first (sub_8284AED8)
constexpr uint32_t kEventFrame = BO2_GAME_ADDR(0x83AF71DC, 0x8372AA14);   // the Backend's "frame ready" event (sub_82428878 sets it)

// --- Layouts -----------------------------------------------------------------------------------------------------
// Queue (sub_8284AAA8): +0 itself, +8 a lock-free list, +48 jobs in it, +52 mask of the hardware threads whose slots
// take from it (sub_82849D08), +56 where its jobs go when it is released (sub_8284AB88), +60 open flag
// (sub_8284AB18), +64 count that must be zero before the release (sub_8284AC48).
constexpr uint32_t kQueueCount = 48;
constexpr uint32_t kQueueMask = 52;
constexpr uint32_t kQueueTarget = 56;
constexpr uint32_t kQueueOpen = 60;
constexpr uint32_t kQueuePending = 64;
// Slot, 128 bytes (sub_8284BC40, sub_8284A878): +4 thread handle, +8 thread id, +20 hardware thread mask, +24 index,
// +28 registered queues, +32 its own queue (so its count is at +80), +88 eight registered queue pointers,
// +120 time spent in jobs (timebase ticks, 64-bit).
constexpr uint32_t kSzSlot = 128;
constexpr uint32_t kSlotMask = 20;
constexpr uint32_t kSlotIndex = 24;
constexpr uint32_t kSlotRegistered = 28;
constexpr uint32_t kSlotOwn = 32;
constexpr uint32_t kSlotQueues = 88;
constexpr int kQueuesPerSlot = 8;
// Job record: +12 type descriptor (+8 function, +16 queued of that type, +20 running), +16 optional group
// (+0 queued, +4 running). The "key" a filtered pick compares is the group, or descriptor + 16 without one
// (sub_8284AD18).
constexpr uint32_t kJobType = 12;
constexpr uint32_t kJobGroup = 16;
// +20 a counter and +24 a target (sub_82492D48's 4th argument): the job runs only once the counter reaches the
// target (unsigned). Until then each thread that picks it pushes it back without running it, checking the
// counter at most every 50,000 timebase ticks (sub_8284AFB8 at 0x8284B0EC, sub_8284B730, sub_8284B2F8).
constexpr uint32_t kJobDependency = 20;
constexpr uint32_t kJobGoal = 24;

// Return addresses of the picker's callers (sub_8284AED8), which tell who is picking.
constexpr uint32_t kChooseWorker1 = BO2_GAME_ADDR(0x8284B054, 0x8262E104);  // worker loop sub_8284AFB8
constexpr uint32_t kChooseWorker2 = BO2_GAME_ADDR(0x8284B2B8, 0x8262E368);
constexpr uint32_t kChooseHelp1 = BO2_GAME_ADDR(0x8284B384, 0x8262E434);       // helper loop sub_8284B2F8 (from sub_8284BB48)
constexpr uint32_t kChooseHelp2 = BO2_GAME_ADDR(0x8284B548, 0x8262E5F8);
constexpr uint32_t kChooseWait1 = BO2_GAME_ADDR(0x8284B80C, 0x8262E8BC);      // "wait for a job type" sub_8284B730
constexpr uint32_t kChooseWait2 = BO2_GAME_ADDR(0x8284BA64, 0x8262EB14);

// Guest words are big-endian. On Win32 and macOS arm64 the area from 0xE0000000 is 0x1000 higher, as in the recompiled code.
inline uint32_t Read32(const uint8_t* base, uint32_t dir) {
  dir += rex::memory::detail::PhysicalHostOffset(dir);
  uint32_t v;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}

// --- Diagnostics (bo2zm_jobs_report_s) ----------------------------------------------------------------------
// Counters per game thread, summed over each report window. All relaxed: the report is a statistic.
struct PerThread {
  const char* label = "?";
  uint32_t id = 0;  // host thread id (as in the log's [tNNNN])
  std::atomic<uint64_t> choose{0};             // picks (sub_8284AED8)
  std::atomic<uint64_t> choose_empty[3] = {};  // empty picks: nothing queued / queued in my queues / only elsewhere
  std::atomic<uint64_t> choose_dependent{0};  // ... that took a job whose dependency is not met (put back)
  std::atomic<uint64_t> yielded{0};            // NtYieldExecution through the game's wrapper (sub_827D8DD8)
  std::atomic<uint64_t> wait_worker{0};  // worker waits (sub_8284A088)
  std::atomic<uint64_t> wait_no_sleep{0};  // ... that returned without waiting because jobs were queued
  std::atomic<uint64_t> ns_wait_worker{0};
  std::atomic<uint64_t> polls{0};            // zero-timeout polls of the frame event (sub_82428ED8)
  std::atomic<uint64_t> polls_ready{0};
  std::atomic<uint64_t> queued{0};          // jobs pushed (sub_8284A910)
  std::atomic<uint64_t> returned{0};          // jobs put back by the loops after a run (sub_8284C3B8)
  std::atomic<uint64_t> helps{0};             // sub_8284BB48 calls
  std::atomic<uint64_t> ns_helps{0};
  std::atomic<uint64_t> waits_type{0};       // sub_8284B730 calls
  std::atomic<uint64_t> ns_waits_type{0};
  std::atomic<uint64_t> stalls{0};           // host blocks added by bo2zm_waits.cpp
  std::atomic<uint64_t> stalls_warned{0};  // ... ended by a notification (not by the timeout)
  std::atomic<uint64_t> ns_stalls{0};
};

// Notifications sent by bo2zm_waits.cpp, by reason (counted for the report).
enum Reason : int {
  kWarningJob,   // a job was pushed, or a released queue's jobs were moved where threads take them
  kWarningDeferred,  // a queue was put on the release list: the next pick releases it (sub_8284AC48)
  kWarningEnded,   // a thread finished (or put back) a job
  kWarningEvent,    // the Backend's frame event or its "frame done" event was set
  kWarningRing,    // the native renderer read further into the ring
  kWarningLock,    // the game's critical section 6 was left (sub_824A0BC0(6))
  kNumWarnings
};
inline std::atomic<uint64_t> g_warnings[kNumWarnings] = {};

// Where bo2zm_waits.cpp sleeps (for the report): sleeps, those ended by the timeout, and time asleep.
enum Room : int {
  kRoomPollFrame, kRoomPollDone, kRoomPollRing, kRoomAfterRelease, kRoomWorker, kRoomType,
  kRoomGroup, kRoomServer, kRoomDependency, kRoomLock, kRoomPutBack, kRoomCull, kNumRooms
};
struct PerSite {
  std::atomic<uint64_t> times{0}, exhausted{0}, ns{0};
};
inline PerSite g_rooms[kNumRooms];

// True when the report is on. Cheap (a static read).
bool ReportActive();
// This thread's counters (registered on first use, with the host thread's name). Only when the report is on.
PerThread& JobThread();
// Starts the report thread on first call (needs guest memory: base).
void Boot(uint8_t* base);
// Classifies an empty pick by slot: 0 nothing queued anywhere, 1 jobs in a queue this slot takes from (lost a race
// or filtered out), 2 jobs only in queues this slot does not take from.
int ClassifyEmpty(const uint8_t* base, uint32_t slot);
void AnnotateQueued(const uint8_t* base, uint32_t job, uint32_t queue);
// A job put back on its queue after a run that returned nonzero (or a dependency check that held it back): the
// loops' own pushes (sub_8284C3B8 from 0x8284B20C, 0x8284B480, 0x8284B9C0).
void AnnotateReturned(uint32_t type);
// A picked job whose dependency counter had not reached its target.
void AnnotateDependency(uint32_t counter, uint32_t type);
// A wait of the game (who: the hooked function's address, lr: its caller) and its duration.
void AnnotateWait(uint32_t who, uint32_t lr, uint64_t ns);

}  // namespace bo2zm::jobs
