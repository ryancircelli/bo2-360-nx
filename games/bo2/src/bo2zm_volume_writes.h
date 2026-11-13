// The model lighting volume's writer, hooked: which pages the game wrote (bo2zm_native_volume_by_writes). See
// bo2zm_volume_writes.cpp.

#pragma once

#include <cstdint>
#include <vector>

namespace bo2zm::volume_writes {

// Whether UpdateVolume finds the changed pages from the writer's marks now (the setting, its A/B factor, and not
// switched off by a miss).
bool UseMarks();

// Appends to `pages` the indices (0 = the page at physical_first) of the marked pages among [physical_first,
// physical_first + count * 4096) and clears their marks. physical_first is page-aligned.
void TakeMarks(uint32_t physical_first, uint32_t count, std::vector<uint32_t>& pages);

// A page changed that no mark covered (the check in UpdateVolume): marks are not used for the rest of the run.
void Missed(uint32_t physical_page);

}  // namespace bo2zm::volume_writes
