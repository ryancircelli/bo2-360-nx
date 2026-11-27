// PC test attenuation never changes a Switch build's normal output gain.
#pragma once
namespace bo2zm::audio {
inline constexpr int OutputVolumePercent(int requested) {
  return requested < 0 ? 0 : requested > 100 ? 100 : requested;
}
}  // namespace bo2zm::audio
