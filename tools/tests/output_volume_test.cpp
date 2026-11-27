// The same policy is used by both executables; Switch ignores PC attenuation.
#include "../../core/bo2zm_output_volume.h"
using bo2zm::audio::OutputVolumePercent;
static_assert(OutputVolumePercent(-1) == 0);
static_assert(OutputVolumePercent(0) == 0);
static_assert(OutputVolumePercent(5) == 5);
static_assert(OutputVolumePercent(50) == 50);
static_assert(OutputVolumePercent(100) == 100);
static_assert(OutputVolumePercent(101) == 100);
int main() {}
