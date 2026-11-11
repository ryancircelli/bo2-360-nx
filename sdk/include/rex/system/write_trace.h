// Write trace: who writes a chosen range of guest physical memory (diagnostic, off unless asked for).
//
// REX_TRACE_WRITE_PAGES=<ranges>   hex ranges, comma separated: START-END (END exclusive) or START+LEN. Physical
//                                  addresses (0x00000000-0x1FFFFFFF) or any physical view (0xA/0xC/0xE...), which
//                                  are converted. Example: REX_TRACE_WRITE_PAGES=12885000-12888000,18af6000+4000
// REX_TRACE_WRITE_DIR=<dir>        where events.log and the page dumps go (else events go to the log only).
// REX_TRACE_WRITE_DUMPS=<n>        page dumps kept at most (default 20000; 0 = none).
// REX_TRACE_WRITE_NOFAULT=1        don't write-protect the guest views (no fault records for raw guest stores).
//
// What is recorded, as one line each:
//   - every explicit write noted by a known writer (Note/NoteGuest): file reads into guest memory with the file,
//     offset, length and which cache served it; the game's copy and fill routines with their guest caller; the
//     stream decoder; GPU memexport readbacks and resolves;
//   - write faults on the watched pages through the guest views (raw guest stores, and native code writing through
//     the virtual views): the host pc and lr, the guest lr of the faulting thread. The pages are write-protected again
//     every millisecond, so the first store after each re-arm is caught;
//   - every content change of a watched 4 KB page (a watcher thread hashes them every millisecond), with a dump of the
//     new contents; a change no noted write or fault explains is flagged "unattributed".

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace rex::memory {
class Memory;
}

namespace rex::write_trace {

extern std::atomic<bool> g_enabled;

// Called once the guest memory is mapped (Memory::Initialize). Reads the environment; does nothing when unset.
void Initialize(memory::Memory* memory);

// The physical address of a guest address in one of the physical views, or the address itself below 0xA0000000
// (taken as physical).
inline uint32_t ToPhysical(uint32_t address) {
  if (address >= 0xA0000000u) {
    return (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0u);
  }
  return address;
}

bool OverlapsPhysical(uint32_t physical, uint32_t length);

// An explicit write of [physical, physical + length), already done. `source` is a short constant tag ("file",
// "memcpy", "lzx", "gpu-memexport", ...); `detail` free text. guest_lr: the guest caller, 0 if none.
void NotePhysicalSlow(const char* source, uint32_t physical, uint32_t length, uint32_t guest_lr,
                      const std::string& detail);

// REX_TRACE_WRITE_FOLLOW is set: writes of 512 bytes or more are checked for followed contents.
bool FollowEnabled();

inline bool Wanted(uint32_t physical, uint32_t length) {
  return g_enabled.load(std::memory_order_relaxed) &&
         ((length >= 512 && FollowEnabled()) || OverlapsPhysical(physical, length));
}

// Guest-address form: only the physical views are of interest.
inline bool WantedGuest(uint32_t guest, uint32_t length) {
  return guest >= 0xA0000000u && Wanted(ToPhysical(guest), length);
}

// Called from the access-violation handler for every write fault inside the guest mapping, before anything else
// handles it. Async-signal-safe: it only appends to a lock-free ring. Returns nothing; never handles the fault.
void OnWriteFault(uintptr_t host_address, uint64_t host_pc, uint64_t host_lr);

// True when the host page of host_address is one this trace write-protected (the stale-protection recovery path
// stays quiet about those).
bool IsArmedHostPage(uintptr_t host_address);

// An optional describer the app registers: appends app-level context for a physical range to `out` (for BO2, the
// streaming pool blocks that overlap it). Called from the watcher thread and from noted writes.
using Describer = void (*)(uint32_t physical, uint32_t length, std::string& out);
void SetDescriber(Describer describer);

// An optional ticker the app registers: called on the watcher thread every loop (~0.5 ms), e.g. to diff app tables.
using Ticker = void (*)();
void SetTicker(Ticker ticker);

// A line in <REX_TRACE_WRITE_DIR>/aux.log (timestamped like events.log): app-wide context that is not about a watched
// range (pool allocations, every memexport). Thread-safe.
void Aux(const std::string& text);

// The last CPU write to each 4 KB physical page (any hooked writer: CRT copies/fills, file reads, LZX), for every
// page while the trace is on: time in 1/4 ms since start (0 = never) and the writer's guest lr.
void StampPhysicalSlow(uint32_t physical, uint32_t length, uint32_t guest_lr);
inline void StampGuest(uint32_t guest, uint32_t length, uint32_t guest_lr) {
  if (guest >= 0xA0000000u && g_enabled.load(std::memory_order_relaxed)) [[unlikely]] {
    StampPhysicalSlow(ToPhysical(guest), length, guest_lr);
  }
}
// The newest stamp over a range: ms (negative when never) and its lr.
double LastCpuWriteMs(uint32_t physical, uint32_t length, uint32_t* guest_lr);
double ElapsedMs();

}  // namespace rex::write_trace
