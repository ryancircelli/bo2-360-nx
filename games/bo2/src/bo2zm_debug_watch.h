// PC diagnostics: who writes a range of guest memory (bo2zm_debug_watch_volume, bo2zm_debug_watch_vertices). See
// bo2zm_debug_watch.cpp.

#pragma once

#include <cstdint>
#include <vector>

namespace bo2zm::debug {

// What the pages are, and the cvar that arms the watch for them.
enum class Watch { kVolume, kVertices };

// With that purpose's cvar on (PC only), write-protects these 4 KB guest physical pages in the guest's virtual views
// of physical memory and logs, every few seconds, the host code (and its callers) that writes them. Only the first
// call that arms it counts; later calls and every other platform do nothing.
// `bytes`, when given, holds how many bytes the caller read from each page (same order): the last report then splits
// them between the pages written during the watch and those never written.
void WatchPhysicalPages(const std::vector<uint32_t>& pages, Watch what = Watch::kVolume,
                        const std::vector<uint64_t>* bytes = nullptr);

// bo2zm_debug_watch_vertices: on, so the renderer gathers the pages its vertex copies read.
bool WatchVertices();

}  // namespace bo2zm::debug
