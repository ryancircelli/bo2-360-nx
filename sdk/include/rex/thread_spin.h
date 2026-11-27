/**
 * Spin-wait detection for guest threads.
 *
 * On the console every guest thread has a hardware thread of its own, so a loop that polls (NtYieldExecution, a wait
 * with a zero timeout, Sleep(0)) until something happens costs nothing. On the host the same loop takes a core from
 * threads that have work. No title list is needed to find these loops: a thread that makes poll calls back to back,
 * with only a few microseconds of other work between them, is spinning. After a run of such calls the poll sleeps for
 * a short, growing time, so the thread leaves its core to the ones that have work; the first call after a real gap
 * of other work starts over.
 *
 * Kept out of rex/thread.h on purpose: that header is in the recompiled game's precompiled header.
 */
#pragma once

#include <cstdint>

namespace rex::thread {

// The detector's state machine, free of clocks and sleeps so it can be tested. One per thread.
struct SpinTracker {
  struct Config {
    uint32_t gap_ns = 200000;      // two polls closer than this (excluding our own sleeps) are part of one spin
    uint32_t threshold = 32;       // polls in a spin before the first backoff
    uint32_t step_us = 10;         // each further poll in the spin sleeps this much longer than the last ...
    uint32_t max_sleep_us = 200;   // ... up to this
  };
  uint64_t last_end_ns = 0;  // when the previous poll returned (after any sleep of ours)
  uint32_t run = 0;          // polls in the current spin

  // A poll starts at now_ns. Returns how long to sleep, in microseconds (0 = just poll).
  uint32_t OnPoll(uint64_t now_ns, const Config& config);
  // The poll (and any sleep) finished at now_ns.
  void OnReturn(uint64_t now_ns) { last_end_ns = now_ns; }
};

// Called by the guest's poll paths (NtYieldExecution, zero-timeout delays and waits). Cheap when spin_wait_backoff is
// off. Sleeps the calling thread when the detector says it is spinning.
void NoteGuestPoll();

// Totals for the log and tests.
struct SpinStats {
  uint64_t polls;
  uint64_t backoffs;
  uint64_t slept_us;
};
SpinStats GetSpinStats();

}  // namespace rex::thread
