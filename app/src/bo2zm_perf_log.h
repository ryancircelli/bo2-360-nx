// Per-frame performance log for test runs: bo2zm_perf_log (or REX_BO2ZM_PERF_LOG) names a file, written in the format
// of docs/mac/perf-log-format.md (version 1) that tools/mac/perf_summary.py reads and tools/mac/boot_test.sh asks for.
//
// The native renderer reports each presented frame from its ring thread (OnPresent) with its own cumulative counters;
// the log turns them into per-frame values. A frame's GPU time is only known when its submissions finish, a frame or
// two later (NoteGpuJob), so frames wait in a short queue until then instead of being written with "-". Lines are
// formatted on the ring thread into a memory buffer and a thread of the log's own writes that buffer to the file once
// a second, so the ring never waits for the disk. With the cvar empty every call returns at its first test.
#pragma once

#include <cstdint>
#include <string_view>

namespace bo2zm::perf_log {

// The renderer's cumulative counters at a presented frame. All in nanoseconds or counts since the start.
struct Counters {
  uint64_t ns_working = 0;  // ring thread reading and executing packets, including the batch running now
  uint64_t ns_waiting_guest = 0;  // of that, in WAIT_REG_MEM waits for the game (not the renderer's own work)
  uint64_t ns_present_wait = 0;  // blocked outside the renderer: output fence, swapchain, frames in flight
  uint64_t draws = 0;
  uint64_t jobs_submitted = 0;  // GPU submissions sent so far, this frame's included
  uint64_t ns_encode_wait = 0;  // of ns_working, waiting for the Metal encode thread to drain
  uint32_t width = 0, height = 0;  // the game's output size
};

// True when the log is on (the cvar is read once).
bool Enabled();
// A presented frame. Ring thread.
void OnPresent(const Counters& counters);
// For the emulated GPU plugins (xenos on Vulkan, Metal), which have no counters of their own: every frame the guest
// presents is logged through rex::graphics::SetSwapHook with its frame time only (cpu, gpu and present wait read "-"
// or 0). renderer is the header's renderer name, e.g. "xenos-vulkan" or "metal". Call once after setup.
void InstallSwapHook(std::string_view renderer);
// A GPU submission finished: its number (the one jobs_submitted counts) and the span of its timestamps, if it had any.
void NoteGpuJob(uint64_t order, uint64_t ns, bool measured);
// A pipeline compiled synchronously on the ring thread, taking ns. Ring thread.
void NoteCompile(uint64_t ns, uint64_t key);
// An #event line at the current time; fields are "key=value" pairs separated by tabs. Any thread.
void Event(std::string_view name, std::string_view fields = {});
// A "# key value" header line (gpu, ...), if the file has not been started yet. Any thread.
void SetMeta(std::string_view key, std::string_view value);
// Writes what is pending and the "# end" line. At shutdown.
void Close();

}  // namespace bo2zm::perf_log
