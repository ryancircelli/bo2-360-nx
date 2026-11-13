// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Reflections drawn on demand: an optional interface the renderer calls. BO2 does not use it; its implementation is
// a set of stubs in bo2zm_render_stubs.cpp.
//
// It is for a game that draws a water reflection pass (a separate small view) every frame even when nothing reads
// it. The renderer records here when the reflection is resolved and when a draw samples it, so the game can skip the
// pass unless it was read recently.
//
// Being read is not enough either. Water is submitted whenever it falls inside the view frustum, even when terrain
// covers it completely, so the reflection could be read every frame with no water on screen. The ring thread
// therefore wraps every draw that samples the reflection in an occlusion query and records whether it left any
// samples.
#pragma once

#include <cstdint>

namespace bo2zm::reflection_demand {

// Guest physical address of the reflection target this interface watches (not a BO2 address). It is checked at run
// time before any frame is skipped.
constexpr uint32_t kAddress = 0x07C5A000;

// From the native renderer (ring thread).
void AnnotateRead();  // a draw samples the reflection texture
void AnnotateCopy();    // the reflection is resolved to kAddress
void AnnotateSwap();     // end of a renderer frame

// Reflection visibility. All from the ring thread.
// Whether to measure: the setting is on and the guard has not tripped. Asked once per frame.
bool MeasureVisibility();
// Whether the witnesses are already verified (visibility decides). Until then the witness is measured
// more often.
bool VisibilityChecked();
// A draw that samples the reflection left samples on screen. measured = false: it could not be measured
// (no room for the query, a game query already open, the deferred sky...) and is treated as visible,
// the safe choice.
void AnnotateVisible(bool measured);
// A draw that samples the reflection left no samples: the terrain or something else covered it entirely.
void AnnotateHidden();
// The witness: the final composite, which covers the whole screen and must leave samples. If it
// ever gives 0, the queries cannot be trusted and the decision falls back to reads.
void AnnotateWitness(bool with_samples);

}  // namespace bo2zm::reflection_demand
