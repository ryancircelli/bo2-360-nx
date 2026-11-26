#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::graphics::metal {

// Merges a completed memexport readback into guest memory one dword at a
// time when the CPU also wrote the range after the export. The CPU's data wins
// only where it changed (guest != guest_snapshot), so a GPU flag exported into
// a structure the CPU keeps updating (MW2012's job-system signal slots) is not
// lost. Dwords still equal to the pre-draw GPU copy (gpu_snapshot, may be
// null) were not exported and may hold stale data, so they are never copied.
// on_copy(d, old_value, new_value) runs for every dword written. Returns the
// number of CPU-written dwords kept.
template <typename OnCopy>
uint32_t MergeMemexportReadback(uint32_t* guest, const uint32_t* gpu,
                                const uint32_t* guest_snapshot,
                                const uint32_t* gpu_snapshot, size_t count,
                                OnCopy&& on_copy) {
  uint32_t cpu_kept = 0;
  for (size_t d = 0; d < count; ++d) {
    const uint32_t old_value = guest[d];
    if (old_value != guest_snapshot[d]) {
      ++cpu_kept;
      continue;
    }
    if (gpu[d] == old_value) continue;
    if (gpu_snapshot && gpu[d] == gpu_snapshot[d]) continue;
    guest[d] = gpu[d];
    on_copy(d, old_value, gpu[d]);
  }
  return cpu_kept;
}

}  // namespace rex::graphics::metal
