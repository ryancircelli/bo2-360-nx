// Entry points the renderer in native/render/ expects to find outside its own files.
//
// They are optional features of the renderer: reflections drawn on demand, a shadow guard for 30 FPS, a scenery LOD
// hook and a readout of the game's anti-aliasing mode. None of them applies to BO2 as it stands, so each one here
// gives the "feature off" answer. The
// pieces BO2 does need have their own implementations elsewhere: the composite marker in bo2zm_d3d_marker.cpp, the
// blocking ring waits in bo2zm_waits.cpp and the game frame counter in bo2zm_d3d_trace.cpp.

#include <atomic>
#include <cstdint>
#include <string>

#include "render/bo2zm_native_hooks.h"
#include "render/bo2zm_native_render.h"
#include "render/bo2zm_reflection_demand.h"

// Game frames, one per Swap (bo2zm_d3d_trace.cpp); only used for a measurement.
std::atomic<uint64_t> g_bo2zm_frames_game{0};

namespace bo2zm::reflection_demand {
void AnnotateRead() {}
void AnnotateCopy() {}
void AnnotateSwap() {}
bool MeasureVisibility() { return false; }  // never measure: the mirror logic stays off
bool VisibilityChecked() { return false; }
void AnnotateVisible(bool) {}
void AnnotateHidden() {}
void AnnotateWitness(bool) {}
}  // namespace bo2zm::reflection_demand

namespace bo2zm::guard30 {
void Heartbeat(double) {}
void Report() {}
bool NoShadows(bool of_user) { return of_user; }  // only the player's own setting
}  // namespace bo2zm::guard30

namespace bo2zm::render_targets {
// Samples per pixel of the game's AA mode, used to scale occlusion query results. BO2 is drawn without MSAA here,
// so this is always 1.
uint32_t SamplesOriginalModeCurrent(const uint8_t*) { return 1; }
}  // namespace bo2zm::render_targets

namespace bo2zm::scenery_lod {
std::string Summary() { return {}; }
}  // namespace bo2zm::scenery_lod

namespace bo2zm::native {
// ActivateConsumerMarkers and AnnotateCheckMarker are implemented by BO2's marker producer
// (games/bo2/src/bo2zm_d3d_marker.cpp). WarnProgressRing wakes the Backend thread's "wait for the ring to drain"
// (games/bo2/src/bo2zm_waits.cpp).
}  // namespace bo2zm::native
