/**
 * @file        system/savepoint.h
 * @brief       Save points step S1/S2: quiesce guest threads and dump guest memory
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

// On-demand same-process save/load for tests ("savepoint save" / "savepoint load" in the test
// control folder). Queues the op for the next command-processor safe point and waits for it.
// Returns "ok save frame=<f> mb=<n> ms=<t>", "ok load frame=<f> ms=<t> loads=<n>" or "error ...".
// A load can be repeated any number of times; a new save replaces the old one.
std::string ControlInProcessSavepoint(const std::string& op, int timeout_ms);

// Fresh-process save points ("savepoint file PATH" in the test control folder, --load_savepoint=PATH).
// The file (REXSP2, savepoint_file.cpp) holds the in-process capture plus the GPU state, and is only
// valid for the same executable (LC_UUID checked on load).
class IGraphicsSystem;
// Implemented by the GPU plugin. op 0 (save, command-processor thread): waits for the GPU, fills `state`
// (register file, ring and interrupt state, last swap) and `gpu_pages` ({u32 physical address, u32 size,
// bytes}... for pages only the GPU copy holds). op 1 (load): applies `state`, invalidates GPU memory and
// presents the saved last frame. op 2 / op 3 (load): hold / release the guest interrupt callback.
using GraphicsFileHook = bool (*)(IGraphicsSystem* graphics, int op, std::vector<uint8_t>& state,
                                  std::vector<uint8_t>& gpu_pages);
void SetGraphicsFileHook(IGraphicsSystem* graphics, GraphicsFileHook hook);
// Command-processor thread: capture in process, then write the file. Returns the control reply.
std::string WriteSavepointFile(const std::string& path, int frame);
// Fresh process, after the module is loaded and before its thread runs. Logs "[savepoint] restored ...".
bool LoadSavepointFile(const std::string& path);
// --load_savepoint at launch: 0 not requested, 1 loaded, -1 failed.
int LoadSavepointAtLaunch();
// Called on each guest thread as it starts: records its executable thread-local block for file save points.
void NoteGuestThreadStart(uint32_t handle);

}  // namespace rex::system
