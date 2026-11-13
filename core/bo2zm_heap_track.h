#pragma once

namespace bo2zm::heap_track {

// Test build only (BO2ZM_HEAP_TRACK, bo2zm_heap_track.cpp): once a minute, the C heap's live bytes by caller to the
// log. Main thread, from the frame hook. Does nothing in other builds.
void EveryFrame();

}  // namespace bo2zm::heap_track
