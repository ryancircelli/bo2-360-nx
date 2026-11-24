/**
 * @file        system/savepoint.h
 * @brief       Save points step S1/S2: quiesce guest threads and dump guest memory
 */

#pragma once

#include <cstdint>
#include <string>

namespace rex::system {

struct SavepointStats {
  bool ok = false;
  uint32_t threads_parked = 0;
  uint32_t regions = 0;
  uint64_t bytes = 0;
  double park_ms = 0;
  double write_ms = 0;
  double resume_ms = 0;
  std::string error;
};

// S2: suspends every running guest thread except the caller's, S1: writes all
// committed guest memory (virtual heaps + physical heap) to `path`, then
// resumes. Call from a host thread at the frame boundary (graphics swap).
// File: "REXSP1\0\0", then repeated {u32 space (0 virtual,1 physical), u32 guest_base,
// u32 size} + size bytes, ended by space = 0xFFFFFFFF.
SavepointStats SaveGuestMemory(const std::string& path);

// Asks the frame boundary to take a savepoint (used by tests and control hooks).
void RequestSavepoint(const std::string& path);
// Counts guest swaps; capture/rewind runs at the next command-processor safe point.
void NoteSavepointSwap();
// Called at the top of the command-processor loop, outside ring packet execution.
void ServiceSavepointRequest();

// S3 (experimental): same-process rewind. CaptureInProcessSavepoint keeps guest memory,
// and for every parked guest thread its host register state and host stack, in RAM.
// RewindToInProcessSavepoint writes them all back while the threads are parked and resumes.
SavepointStats CaptureInProcessSavepoint();
SavepointStats RewindToInProcessSavepoint();
void SavepointCrashDiff(uint32_t fault, uint32_t r3, uint32_t r4);
// Diagnostic: REX_SAVEPOINT_TRACE_CS=<hex guest address> records every enter of that critical section
// after the capture (thread, guest lr, return chain, r3-r5/r28-r31) and dumps it on a guest crash.
void SavepointTraceCs(uint32_t cs);

// On-demand same-process save/load for tests ("savepoint save" / "savepoint load" in the test
// control folder). Queues the op for the next command-processor safe point and waits for it.
// Returns "ok save frame=<f> mb=<n> ms=<t>", "ok load frame=<f> ms=<t> loads=<n>" or "error ...".
// A load can be repeated any number of times; a new save replaces the old one.
std::string ControlInProcessSavepoint(const std::string& op, int timeout_ms);

}  // namespace rex::system
