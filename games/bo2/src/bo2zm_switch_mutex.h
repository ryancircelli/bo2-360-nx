// Switch only (bo2zm_switch_mutex.cpp): how contended host locks wait.

#pragma once

namespace bo2zm {
// The C heap's lock spins briefly before it sleeps in the kernel (off by default; bo2zm_malloc_spin).
void SetMallocSpin(bool on);
// bo2zm_malloc_spin_ab: the benchmark A/B factor (1 or 2) whose on arms spin; 0 = not part of the A/B.
void SetMallocSpinFactor(int factor);
}  // namespace bo2zm
