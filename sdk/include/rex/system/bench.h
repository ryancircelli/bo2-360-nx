// Deterministic benchmark window (bench_frames): counts the work of exactly N presented frames, measured inside the
// process at the frame boundaries: instructions, cycles and CPU time per thread (and P/E core split), GPU time of the
// command buffers. With clock_frame_lock_hz the game does the same work per frame however slow the frames are, so
// these totals compare builds even when several runs share the machine. Results: the log line "[bench] done" and
// the JSON file bench_out.
#pragma once

#include <cstdint>

namespace rex::bench {

// Start the window bench_warmup_frames frames from now (the app calls this where its scene begins, e.g. a route's
// start or a restored save point). Only the first call counts. No-op with bench_frames = 0.
void Arm(const char* reason);
// Each frame the guest presents (NotifySwap): steps the frame-locked clock and drives the window.
void OnFrame();
// GPU time of one finished command buffer.
void AddGpuNs(uint64_t ns);
bool Enabled();

}  // namespace rex::bench
