// Frame dumps for comparing renderers (metal_frame_dump).
//
// Saves the guest output (the image the game swaps to the screen, before host
// scaling) of chosen guest frames in the layout of the SDK GPU test harness
// replay directory (tools/gputest/replay/README.md on branch gpu-test):
// summary.json, draws.jsonl with one record per saved frame and a single
// target named "frame", and draws/NNNNNN_frame.npy (float32 HxWx4, 0..1).
// replay_diff.py can then compare two runs (two backends, or before and after
// a change) frame by frame.

#pragma once

#include <cstdint>

namespace rex::ui {
class Presenter;
}

namespace rex::graphics::metal {

// Starts the dump thread when the metal_frame_dump cvar lists frames.
// Call once the presenter exists. title_id goes into summary.json.
void StartFrameDump(ui::Presenter* presenter, uint32_t title_id);

}  // namespace rex::graphics::metal
