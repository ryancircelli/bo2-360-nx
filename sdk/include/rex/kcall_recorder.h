/**
 * @file        kcall_recorder.h
 * @brief       Kernel-call flight recorder: a per-thread ring of recent kernel export calls.
 *
 * Off by default. Turn it on with the environment variable REX_KCALL_RING=<entries per thread>
 * (rounded up to a power of two). When on, every kernel export (REX_EXPORT, REX_EXPORT_STUB*)
 * records its name, arguments, guest return address, time and result into a ring
 * owned by the calling thread, and object-table handle changes go into one shared ring.
 *
 * The rings are written out as plain text (kcall-<reason>-<n>.txt next to the log file) when the
 * process aborts (REX_FATAL, std::abort), hits a trap (SIGTRAP, SIGILL), takes a fatal SIGSEGV or
 * SIGBUS, calls an invalid guest function, when frames stop for kcall_stall_dump_s seconds, on
 * SIGINFO (Ctrl-T or `kill -INFO <pid>`), or when a file named kcall-dump-request appears in the
 * output folder.
 *
 * Off cost: one load and one predictable branch per kernel call.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <tuple>
#include <type_traits>

struct PPCContext;

namespace rex::kcall {

struct Entry;
struct Decode;

// One per hooked export, a function-local static in the hook body (constant-initialised).
struct Site {
  const char* name;
  uint8_t arity;     // number of register arguments recorded (capped at 8)
  bool returns;      // false for void exports (r3 is not a result)
  std::atomic<const Decode*> decode{nullptr};  // resolved on first recorded call
  std::atomic<uint64_t> calls{0};               // recorded or skipped, only counted when on

  constexpr Site(const char* n, uint8_t a, bool r) : name(n), arity(a), returns(r) {}
};

// Entries per thread; 0 = off. Set once before guest code runs.
extern uint32_t g_ring_entries;

Entry* Begin(Site& site, PPCContext& ctx, uint8_t* base, uint64_t* seq_out);
void End(Entry* entry, uint64_t seq, Site& site, PPCContext& ctx, uint8_t* base);

// Object-table hooks (handle lifetimes). op: 'A'dd, 'R'emove, '+' retain, '-' release.
void NoteHandle(char op, uint32_t handle, uint32_t object_type, int32_t ref_count);
inline void HandleEvent(char op, uint32_t handle, uint32_t object_type, int32_t ref_count) {
  if (__builtin_expect(g_ring_entries != 0, 0)) NoteHandle(op, handle, object_type, ref_count);
}

// Records why the process is about to die; the abort handler prints it in the dump header.
void NoteFatal(const char* reason);

// Writes a dump now (from a normal thread). Returns the file path written, or nullptr.
const char* DumpNow(const char* reason);

// Writes the one crash dump of the process (later crash triggers are ignored). DumpFromSignal is
// for fatal signal paths that bypass our own handlers (SEH re-raise, unhandled host exceptions).
void DumpFatal(const char* reason);
void DumpFromSignal(int sig);

class Scope {
 public:
  Scope(Site& site, PPCContext& ctx, uint8_t* base) : site_(site), ctx_(ctx), base_(base) {
    if (__builtin_expect(g_ring_entries != 0, 0)) entry_ = Begin(site, ctx, base, &seq_);
  }
  ~Scope() {
    if (__builtin_expect(entry_ != nullptr, 0)) End(entry_, seq_, site_, ctx_, base_);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Site& site_;
  PPCContext& ctx_;
  uint8_t* base_;
  Entry* entry_ = nullptr;
  uint64_t seq_ = 0;
};

}  // namespace rex::kcall
