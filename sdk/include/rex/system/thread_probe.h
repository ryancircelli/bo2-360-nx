// Guest thread timeline probe (diagnostic, off unless REX_THREAD_PROBE=<path> is set).
//
// Writes one binary stream of fixed 24-byte records: guest wait begin/end (object, caller lr),
// a 1 kHz CPU/run-state sample of every registered thread, guest VdSwap calls, ring swaps,
// vblanks, graphics interrupt callbacks and EVENT_WRITE_SHD fence writes. Thread names go to
// <path>.threads. tools/thread_probe.py (scratch) turns the stream into per-frame timelines.
#pragma once

#include <cstdint>

namespace rex::thread_probe {

enum Kind : uint8_t {
  kWaitBegin = 1,  // obj = object handle, b = object type, a = WaitSub, lr = guest caller
  kWaitEnd = 2,    // a = result (low byte of X_STATUS)
  kSample = 3,     // obj = cpu us since last sample, a = mach run_state, b = 1 if in guest wait
  kGuestSwap = 4,  // guest thread called VdSwap
  kRingSwap = 5,   // command processor executed the swap packet, obj = counter
  kVblank = 6,     // obj = vblank counter after increment
  kIntrBegin = 7,  // obj = source
  kIntrEnd = 8,
  kFence = 9,      // obj = guest address, lr = value written
  kRingIdle = 10,  // command processor found the ring empty, a = 1 begin / 0 end
};

enum WaitSub : uint8_t {
  kSingle = 0,
  kMultiAny = 1,
  kMultiAll = 2,
  kDelay = 3,  // obj = timeout ms
  kYield = 4,
  kSignalAndWait = 5,
};

extern bool g_on;
inline bool On() { return g_on; }

// Call from the thread itself (names the record stream id after `id`).
void RegisterCurrent(uint32_t id, const char* name);
void Emit(uint8_t kind, uint8_t a = 0, uint16_t b = 0, uint32_t obj = 0, uint32_t lr = 0);
// Same as Emit but with an explicit thread id (for host threads without a guest handle).
void EmitAs(uint32_t id, uint8_t kind, uint8_t a, uint16_t b, uint32_t obj, uint32_t lr);
// Records a (re)name for a thread id in <path>.threads.
void NoteName(uint32_t id, const char* name);
// Marks the current thread as inside / outside a guest wait (used by the sampler flag).
void SetWaiting(bool waiting);

}  // namespace rex::thread_probe
