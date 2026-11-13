// A sampling profiler for the PC build (bo2zm_pc_profile, off by default; see bo2zm_pc_profiler.cpp). Nothing on the
// Switch, which has its own (sdk/src/ui/switch_perf.cpp).
#pragma once

namespace bo2zm::pc_profiler {

// Starts the sampling thread if bo2zm_pc_profile is set.
void Start();

// A frame was presented (the native renderer's Present): bo2zm_pc_profile_stalls keeps only the samples taken while
// no frame has been presented for a while.
void NoteFrame();

}  // namespace bo2zm::pc_profiler
